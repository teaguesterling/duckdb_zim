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
// Memory policy (DESIGN §3.4, revised by #27): OpenFile no longer materializes
// the entry. The handle holds a ZimContentReader — a pinned libzim item — and
// every Read pulls only the requested window via libzim's ranged getData, so a
// handle costs O(1) regardless of entry size.
//
// Be precise about what that buys. A consumer doing ranged reads never holds the
// whole entry: opening a 268 MB entry and reading one 1 MiB window measured
// +2 MB of peak RSS, against +524 MB before. But read_blob/read_text read the
// *whole* file into a DuckDB buffer regardless — for those the win is only that
// the extension no longer keeps a second full copy alongside DuckDB's (measured
// 550 MB -> 417 MB peak on that same entry). It stays O(entry), not O(1).
// (libzim separately caches the decompressed cluster in its own LRU, before and
// after this change alike.)
//
// The decompression-bomb cap (zim_max_content_size) still applies at OpenFile,
// against the entry's declared size: a ranged read is self-bounding, but the
// dominant consumers materialize the entry anyway in an allocation this
// extension does not control, so dropping the open-time check would relocate an
// unbounded allocation into DuckDB rather than remove it.
//
// The archive component must be a local libzim-openable file — libzim mmaps it
// directly, so `zim://` cannot currently nest another VFS (S3, http, …) for the
// archive itself.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp" // duckdb::Glob (the matcher)
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

#include <cctype>
#include <cstring>
#include <memory>

namespace duckdb {

using zim_ext::GetArchivePool;
using zim_ext::NormalizeContentPath;
using zim_ext::ScanSpec;
using zim_ext::ZimArchive;
using zim_ext::ZimContentReader;
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

// A ZIM entry served by ranged reads. `reader` pulls windows out of the entry on
// demand instead of holding its bytes; `size` is the entry's decompressed size,
// resolved once at open; `position` is the sequential read cursor.
//
// `archive` is the pool's shared_ptr, held for the handle's whole lifetime: the
// reader addresses data inside that archive, so it must not be reclaimed while a
// file is open. Declaration order matters — `archive` and `reader` are
// constructed before `size` reads back from `reader`.
class ZimFileHandle : public FileHandle {
public:
	ZimFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, std::shared_ptr<ZimArchive> archive_p,
	              ZimContentReader reader_p)
	    : FileHandle(fs, path, flags), archive(std::move(archive_p)), reader(std::move(reader_p)),
	      size(static_cast<idx_t>(reader.Size())), position(0) {
	}
	~ZimFileHandle() override = default;
	void Close() override {
	}

	std::shared_ptr<ZimArchive> archive; // keeps the archive alive under `reader`
	ZimContentReader reader;
	idx_t size;
	idx_t position;
};

class ZimFileSystem : public FileSystem {
public:
	// Captures its owning DatabaseInstance (the VFS is registered per-DB) so the
	// context-free FileSystem methods below can reach that DB's archive pool. The
	// reference is only dereferenced while serving queries, never at teardown.
	explicit ZimFileSystem(DatabaseInstance &db) : db(db) {
	}

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
		auto archive = GetArchivePool(db).Get(parsed.archive);
		// Decompression-bomb guard: cap the decompressed entry size. Resolve the
		// setting through the opener (the VFS has no ClientContext); fall back to the
		// default when no opener is available.
		uint64_t max_content = zim_ext::DEFAULT_MAX_CONTENT_SIZE;
		Value cap_val;
		if (FileOpener::TryGetCurrentSetting(opener, "zim_max_content_size", cap_val) && !cap_val.IsNull()) {
			max_content = cap_val.GetValue<uint64_t>();
		}
		// OpenContent follows redirects (a redirect entry serves its target's
		// bytes, like a symlink), returns nullopt only when the path is absent, and
		// throws when the entry's declared size is over `max_content` — all without
		// reading a byte of the body.
		auto reader = archive->OpenContent(parsed.content, max_content);
		if (!reader) {
			if (flags.ReturnNullIfNotExists()) {
				return nullptr;
			}
			throw IOException("No entry '%s' in ZIM archive '%s'", parsed.content, parsed.archive);
		}
		return make_uniq<ZimFileHandle>(*this, path, flags, std::move(archive), std::move(*reader));
	}

	// Positional read: exact, does not move the cursor (matches LocalFileSystem).
	// Out-of-range is an error, as before — the bounds check is written to avoid
	// the overflow the old `location + nr_bytes` form could hit on a huge offset.
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		auto &h = handle.Cast<ZimFileHandle>();
		const idx_t want = static_cast<idx_t>(nr_bytes);
		if (nr_bytes < 0 || location > h.size || want > h.size - location) {
			throw IOException("Read of %lld bytes at %llu exceeds zim:// file size %llu (%s)",
			                  static_cast<long long>(nr_bytes), static_cast<unsigned long long>(location),
			                  static_cast<unsigned long long>(h.size), h.path);
		}
		// One ranged read normally satisfies this; the loop only matters if libzim
		// ever returns a short window, in which case a short read is an error here.
		idx_t done = 0;
		while (done < want) {
			const uint64_t got = h.reader.ReadAt(location + done, want - done, static_cast<char *>(buffer) + done);
			if (got == 0) {
				throw IOException("Short read of %lld bytes at %llu from zim:// file (%s)",
				                  static_cast<long long>(nr_bytes), static_cast<unsigned long long>(location), h.path);
			}
			done += static_cast<idx_t>(got);
		}
	}

	// Sequential read: clamps at EOF and advances the cursor.
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &h = handle.Cast<ZimFileHandle>();
		if (nr_bytes <= 0 || h.position >= h.size) {
			return 0;
		}
		const int64_t available = static_cast<int64_t>(h.size - h.position);
		const int64_t to_read = MinValue<int64_t>(nr_bytes, available);
		const uint64_t got = h.reader.ReadAt(h.position, static_cast<uint64_t>(to_read), static_cast<char *>(buffer));
		h.position += static_cast<idx_t>(got);
		return static_cast<int64_t>(got);
	}

	int64_t GetFileSize(FileHandle &handle) override {
		return static_cast<int64_t>(handle.Cast<ZimFileHandle>().size);
	}

	// Modification time of a zim:// entry == mtime of the archive file that contains
	// it. A ZIM entry has no independent mtime: the archive is a single immutable
	// container, written once, so every entry inside it shares the container's
	// modification time. That is also the answer a caching consumer wants — replacing
	// the .zim file on disk is the only way an entry's bytes can change, so the
	// container's mtime is exactly the invalidation signal.
	//
	// Without this override the base FileSystem throws NotImplementedException, which
	// blocks every consumer that stats a file before reading it — read_parquet most
	// visibly, i.e. precisely the positional-read consumers the ranged-read VFS exists
	// to serve.
	//
	// KNOWN INCONSISTENCY (not fixed here; needs its own change): ArchivePool caches an
	// opened ZimArchive and never revalidates it against the file on disk. If the .zim
	// is replaced while a pooled handle is still live, reads keep being served from the
	// OLD archive while this method reports the NEW file's mtime — the two disagree, and
	// a consumer that trusts the mtime to mean "contents changed" would see stale bytes
	// under a fresh timestamp. Fixing that belongs in the pool (stat-on-Get and reopen
	// when dev/inode/mtime/size moved), not here.
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		ParsedZimPath parsed;
		if (!TryParseZimUrl(handle.path, parsed)) {
			throw IOException("Invalid zim:// path '%s' (expected zim://<archive>.zim/<content-path>)", handle.path);
		}
		// Stat through the DB's FileSystem rather than a raw stat(): the archive may
		// live behind another registered VFS, and this keeps the one open-and-stat
		// path DuckDB already uses. No libzim involvement — this is a file stat, not
		// an archive read, so it deliberately does not go through the archive pool.
		auto &fs = FileSystem::GetFileSystem(db);
		auto archive_handle = fs.OpenFile(parsed.archive, FileFlags::FILE_FLAGS_READ);
		return fs.GetLastModifiedTime(*archive_handle);
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
			auto archive = GetArchivePool(db).Get(parsed.archive);
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
			archive = GetArchivePool(db).Get(parsed.archive);
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

private:
	DatabaseInstance &db; // owning DB; source of this VFS's archive pool
};

} // namespace

void RegisterZimFilesystem(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	FileSystem::GetFileSystem(db).RegisterSubSystem(make_uniq<ZimFileSystem>(db));
}

} // namespace duckdb
