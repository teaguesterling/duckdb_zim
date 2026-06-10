//===----------------------------------------------------------------------===//
// zim_access.hpp
//
// Thin wrapper over libzim that encodes the *verified* libzim-7 semantics
// (see docs/libzim-semantics.md). All libzim contact for the extension is
// confined here and in zim_archive_pool.*; the DuckDB binding layer
// (read_zim.cpp, zim_metadata.cpp, zim_scalars.cpp) consumes only the plain
// structs below and never touches a zim:: type directly.
//
// Key invariants baked in here, from the oracle run:
//   * Content paths are namespace-free. A leading "C/" is tolerated on lookup
//     and stripped. "M/" / "W/" / "X/" are NOT resolvable via path lookup.
//   * read_zim's universe is the content (C) namespace: iterByPath / findByPath.
//   * Metadata is a separate key space: getMetadataKeys / getMetadata.
//   * getMainEntry() may itself be a redirect; resolve it.
//   * Content is binary -> std::string of bytes -> DuckDB BLOB.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zim {
class Archive; // fwd; real include only in zim_access.cpp
}

namespace duckdb {
namespace zim_ext {

// One row of the content scan. `content` is populated lazily and only when the
// caller asks for it (projection pushdown): a listing scan must never pay the
// decompression cost for blobs it won't emit.
struct ZimEntry {
	std::string path; // namespace-free content path, e.g. "A/Photosynthesis"
	std::string title;
	std::string mimetype; // empty for redirects
	bool is_redirect = false;
	std::string redirect_path; // target content path; empty unless is_redirect
	uint64_t size = 0;         // item size in bytes; 0 for redirects
	std::string content;       // raw bytes; only filled when want_content was set
	bool content_loaded = false;
};

// Archive-level structural facts (distinct from M-namespace metadata).
struct ZimInfo {
	std::string uuid;
	uint64_t entry_count = 0;     // content/user entries
	uint64_t all_entry_count = 0; // + metadata + well-known + index
	uint64_t article_count = 0;
	uint64_t media_count = 0;
	std::string main_entry_path; // redirect-resolved landing path
	bool has_fulltext_index = false;
	bool has_title_index = false;
	bool has_checksum = false;
	bool is_multipart = false;
	bool has_new_namespace_scheme = false;
	uint64_t filesize = 0;
};

struct ZimSearchHit {
	std::string path;
	std::string title;
	std::optional<double> score; // NULL when libzim build doesn't expose it
	std::optional<std::string> snippet;
};

// How a content scan should be ordered / scoped. Drives the choice of the
// underlying libzim range (iterByPath/iterByTitle/findByPath/findByTitle).
enum class ScanOrder { ByPath, ByTitle };

struct ScanSpec {
	ScanOrder order = ScanOrder::ByPath;
	std::optional<std::string> path_prefix;  // -> findByPath
	std::optional<std::string> title_prefix; // -> findByTitle
	// Accept-style mimetype row filter (post-filter; libzim has no mimetype index).
	// Empty = no filter. Patterns may be exact (text/html), a subtype wildcard
	// (image/*), or the catch-alls */* and *. Redirects (no mimetype) never match.
	std::vector<std::string> mimetype_filter;
	bool want_content = false; // fetch blobs during the scan
	// When want_content and this is non-empty, the blob is loaded ONLY for
	// entries whose mimetype is listed here; others come back content_loaded =
	// false (NULL content). Empty = load every scanned entry. This is what lets
	// a bulk scan materialize, say, just text/html without paying to decompress
	// the images alongside them.
	std::vector<std::string> content_mimetypes;
};

// Forward-only cursor over content entries. Wraps a libzim EntryRange iterator
// pair; defined in the .cpp so this header stays free of <zim/*> includes.
class ZimScanCursor {
public:
	~ZimScanCursor();
	ZimScanCursor(ZimScanCursor &&) noexcept;
	ZimScanCursor &operator=(ZimScanCursor &&) noexcept;
	ZimScanCursor(const ZimScanCursor &) = delete;
	ZimScanCursor &operator=(const ZimScanCursor &) = delete;

	// Advances and fills `out`. Returns false at end of range. Applies the
	// mimetype post-filter and (when want_content) loads the blob.
	bool Next(ZimEntry &out);

private:
	friend class ZimArchive;
	struct Impl;
	explicit ZimScanCursor(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

// RAII handle around a single open zim::Archive. Cheap to copy by shared_ptr
// via the ArchivePool; never construct directly outside the pool in the binding
// layer (the pool keeps the libzim cluster cache warm across queries).
class ZimArchive {
public:
	// Throws std::runtime_error (wrapping zim::ZimFileFormatError etc.) on failure.
	static std::shared_ptr<ZimArchive> Open(const std::string &file_path);
	~ZimArchive();

	const std::string &FilePath() const {
		return file_path_;
	}

	// --- archive-level info -------------------------------------------------
	ZimInfo Info() const;

	// --- content lookups (C namespace) -------------------------------------
	// `want_content` controls whether the blob is loaded. A leading "C/" in
	// `path` is stripped before lookup. Returns nullopt when absent.
	bool HasEntry(const std::string &path) const;
	std::optional<ZimEntry> GetByPath(const std::string &path, bool want_content) const;
	std::optional<ZimEntry> GetByTitle(const std::string &title, bool want_content) const;
	// Just the bytes for a content path (redirect-following). nullopt if absent.
	std::optional<std::string> GetContent(const std::string &path) const;
	std::optional<std::string> GetMimetype(const std::string &path) const;
	std::optional<std::string> GetRedirectTarget(const std::string &path) const;
	std::string MainEntryPath() const; // redirect-resolved; "" if none

	// --- content scan / listing --------------------------------------------
	ZimScanCursor Scan(const ScanSpec &spec) const;

	// --- metadata (M namespace) --------------------------------------------
	std::vector<std::string> MetadataKeys() const;
	std::optional<std::string> Metadata(const std::string &key) const; // value bytes
	std::optional<std::string> MetadataMimetype(const std::string &key) const;
	// Parsed `Counter` metadata: mimetype -> count. Empty if no Counter key.
	std::map<std::string, int64_t> Counter() const;

	// --- full-text search (optional libzim feature) ------------------------
	bool HasFulltextIndex() const;
	// Returns hits [offset, offset+limit). Empty when no fulltext index.
	std::vector<ZimSearchHit> Search(const std::string &query, uint32_t offset, uint32_t limit) const;

	// --- title autocomplete ------------------------------------------------
	// Ranked title suggestions [offset, offset+limit). Backed by libzim's
	// SuggestionSearcher where xapian is present; degrades to a findByTitle
	// prefix listing otherwise (so it works on search-less builds too).
	std::vector<ZimSearchHit> Suggest(const std::string &query, uint32_t offset, uint32_t limit) const;

	// --- archive-level utilities -------------------------------------------
	// Illustration (favicon / cover) PNG bytes of the requested square size;
	// nullopt when the archive has no illustration at that size.
	std::optional<std::string> Illustration(unsigned int size) const;
	// A random content entry's path; "" when the archive has no content entries.
	std::string RandomPath() const;
	// libzim's archive integrity check (checksum + structural validation).
	bool CheckIntegrity() const;

private:
	explicit ZimArchive(const std::string &file_path);
	std::string file_path_;
	std::unique_ptr<zim::Archive> archive_; // owns the warm cluster cache
};

// Strips a single leading "C/" (the only namespace prefix the high-level API
// tolerates) so callers may pass either form. Exposed for the binding layer's
// path normalization and unit tests.
std::string NormalizeContentPath(const std::string &path);

// Accept-style mimetype match: true if `patterns` is empty (no filter) or
// `mimetype` matches any pattern. Patterns support exact (text/html), subtype
// wildcard (image/*), and the catch-alls */* and *. An empty mimetype (a
// redirect) never matches a non-empty pattern set. Shared by the row filter
// and the include_content content gate.
bool MimetypeAccepted(const std::vector<std::string> &patterns, const std::string &mimetype);

} // namespace zim_ext
} // namespace duckdb
