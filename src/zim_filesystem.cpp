//===----------------------------------------------------------------------===//
// zim_filesystem.cpp — the `zim://` virtual filesystem (DESIGN §3, phase 2).
//
// Registers a read-only DuckDB FileSystem that addresses entries *inside* a ZIM
// archive by content path, so the whole extension ecosystem (webbed, read_html,
// read_blob, read_text, …) can read ZIM contents with zero coupling and no GPL
// linkage back into those MIT extensions:
//
//     SELECT * FROM read_html('zim://wikipedia.zim/A/Photosynthesis');
//     SELECT * FROM read_blob('zim://wikipedia.zim/I/*');
//
// Grammar (content-path-first, per docs/libzim-semantics.md — the namespace-led
// grammar in DESIGN §3.1 was dropped once verification showed libzim 7's high
// level API does not resolve M/W/X paths):
//
//     zim://<archive-path>.zim/<content-path>
//
// Boundary detection: the archive component is the longest prefix ending in
// `.zim` (or a split-archive suffix `.zim[a-z][a-z]`) that is followed by `/`;
// everything after that `/` is the content path. A leading `C/` on the content
// path is tolerated and stripped (NormalizeContentPath).
//
// Memory policy (DESIGN §3.4): a ZIM item is a decompressed blob, not a seekable
// byte range, so OpenFile *materializes* the entry into the handle and serves
// reads from memory. Fine for articles; a caller pulling very large media pays
// that blob's size in RAM. The archive component must be a local libzim-openable
// file — libzim mmaps it directly, so `zim://` cannot currently nest another VFS
// (S3, http, …) for the archive itself.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp" // duckdb::Glob (the matcher)
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

#include <cctype>
#include <cstring>

namespace duckdb {

using zim_ext::ArchivePool;
using zim_ext::NormalizeContentPath;
using zim_ext::ScanSpec;
using zim_ext::ZimArchive;
using zim_ext::ZimEntry;

namespace {

constexpr const char *ZIM_SCHEME = "zim://";

// archive = the on-disk libzim file; content = the namespace-free path within it.
struct ParsedZimPath {
	string archive;
	string content;
};

// Splits a zim:// URL at the archive boundary. Returns false if `url` is not a
// zim:// URL or has no `.zim/` boundary. `content` may be empty (the archive
// root, which addresses no file).
bool TryParseZimUrl(const string &url, ParsedZimPath &out) {
	if (!StringUtil::StartsWith(url, ZIM_SCHEME)) {
		return false;
	}
	const string rest = url.substr(strlen(ZIM_SCHEME));
	const string lower = StringUtil::Lower(rest);

	idx_t search = 0;
	while (true) {
		auto dot = lower.find(".zim", search);
		if (dot == string::npos) {
			return false;
		}
		const idx_t after = dot + 4; // index just past ".zim"
		// Plain single-file archive: ".zim/"
		if (after < rest.size() && rest[after] == '/') {
			out.archive = rest.substr(0, after);
			out.content = rest.substr(after + 1);
			return true;
		}
		// Split archive: ".zim" + two letters + "/" (e.g. wikipedia.zimaa/…).
		if (after + 2 < rest.size() && std::isalpha(static_cast<unsigned char>(rest[after])) &&
		    std::isalpha(static_cast<unsigned char>(rest[after + 1])) && rest[after + 2] == '/') {
			out.archive = rest.substr(0, after + 2);
			out.content = rest.substr(after + 3);
			return true;
		}
		search = dot + 4;
	}
}

// First index of a glob metacharacter, or string length if none.
idx_t FirstGlobChar(const string &s) {
	auto pos = s.find_first_of("*?[");
	return pos == string::npos ? s.size() : pos;
}

// A materialized ZIM entry served from memory. `data` holds the (already
// decompressed) blob; `position` is the sequential read cursor.
class ZimFileHandle : public FileHandle {
public:
	ZimFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, string data_p)
	    : FileHandle(fs, path, flags), data(std::move(data_p)), position(0) {
	}
	~ZimFileHandle() override = default;
	void Close() override {
	}

	string data;
	idx_t position;
};

class ZimFileSystem : public FileSystem {
public:
	bool CanHandleFile(const string &fpath) override {
		return StringUtil::StartsWith(fpath, ZIM_SCHEME);
	}

	string GetName() const override {
		return "ZimFileSystem";
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		if (flags.OpenForWriting() || flags.OpenForAppending() || flags.CreateFileIfNotExists() ||
		    flags.OverwriteExistingFile()) {
			throw NotImplementedException("zim:// filesystem is read-only (cannot open '%s' for writing)", path);
		}
		ParsedZimPath parsed;
		if (!TryParseZimUrl(path, parsed) || parsed.content.empty()) {
			throw IOException("Invalid zim:// path '%s' (expected zim://<archive>.zim/<content-path>)", path);
		}
		auto archive = ArchivePool::Instance().Get(parsed.archive);
		// GetContent follows redirects (a redirect entry serves its target's
		// bytes, like a symlink) and returns nullopt only when the path is absent.
		auto data = archive->GetContent(parsed.content);
		if (!data) {
			if (flags.ReturnNullIfNotExists()) {
				return nullptr;
			}
			throw IOException("No entry '%s' in ZIM archive '%s'", parsed.content, parsed.archive);
		}
		return make_uniq<ZimFileHandle>(*this, path, flags, std::move(*data));
	}

	// Positional read: exact, does not move the cursor (matches LocalFileSystem).
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		auto &h = handle.Cast<ZimFileHandle>();
		if (location + static_cast<idx_t>(nr_bytes) > h.data.size()) {
			throw IOException("Read of %lld bytes at %llu exceeds zim:// file size %llu (%s)",
			                  static_cast<long long>(nr_bytes), static_cast<unsigned long long>(location),
			                  static_cast<unsigned long long>(h.data.size()), h.path);
		}
		std::memcpy(buffer, h.data.data() + location, static_cast<size_t>(nr_bytes));
	}

	// Sequential read: clamps at EOF and advances the cursor.
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &h = handle.Cast<ZimFileHandle>();
		if (h.position >= h.data.size()) {
			return 0;
		}
		const int64_t available = static_cast<int64_t>(h.data.size() - h.position);
		const int64_t to_read = MinValue<int64_t>(nr_bytes, available);
		std::memcpy(buffer, h.data.data() + h.position, static_cast<size_t>(to_read));
		h.position += static_cast<idx_t>(to_read);
		return to_read;
	}

	int64_t GetFileSize(FileHandle &handle) override {
		return static_cast<int64_t>(handle.Cast<ZimFileHandle>().data.size());
	}

	void Seek(FileHandle &handle, idx_t location) override {
		handle.Cast<ZimFileHandle>().position = location;
	}
	idx_t SeekPosition(FileHandle &handle) override {
		return handle.Cast<ZimFileHandle>().position;
	}
	void Reset(FileHandle &handle) override {
		handle.Cast<ZimFileHandle>().position = 0;
	}
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override {
		ParsedZimPath parsed;
		if (!TryParseZimUrl(filename, parsed) || parsed.content.empty()) {
			return false;
		}
		try {
			auto archive = ArchivePool::Instance().Get(parsed.archive);
			return archive->HasEntry(parsed.content);
		} catch (...) {
			return false;
		}
	}

	// Path-pattern listing. A literal (non-wildcard) path returns the single
	// entry if present — the multi-file reader treats an empty Glob as
	// "not found", so this MUST return the lone file. A wildcard scans the
	// archive from the literal prefix and matches full content paths.
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		vector<OpenFileInfo> result;
		ParsedZimPath parsed;
		if (!TryParseZimUrl(path, parsed) || parsed.content.empty()) {
			return result;
		}
		std::shared_ptr<ZimArchive> archive;
		try {
			archive = ArchivePool::Instance().Get(parsed.archive);
		} catch (...) {
			return result; // unopenable archive -> no matches
		}
		const string url_prefix = string(ZIM_SCHEME) + parsed.archive + "/";
		const string pattern = NormalizeContentPath(parsed.content);

		if (!FileSystem::HasGlob(parsed.content)) {
			if (archive->HasEntry(pattern)) {
				result.emplace_back(url_prefix + pattern);
			}
			return result;
		}

		ScanSpec spec;
		const string literal = pattern.substr(0, FirstGlobChar(pattern));
		if (!literal.empty()) {
			spec.path_prefix = literal;
		}
		auto cursor = archive->Scan(spec);
		ZimEntry e;
		while (cursor.Next(e)) {
			if (duckdb::Glob(e.path.c_str(), e.path.size(), pattern.c_str(), pattern.size())) {
				result.emplace_back(url_prefix + e.path);
			}
		}
		return result;
	}
};

} // namespace

void RegisterZimFilesystem(ExtensionLoader &loader) {
	auto &fs = FileSystem::GetFileSystem(loader.GetDatabaseInstance());
	fs.RegisterSubSystem(make_uniq<ZimFileSystem>());
}

} // namespace duckdb
