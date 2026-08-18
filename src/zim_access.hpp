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
class FileSystem; // fwd; only consulted for remote opens (zim_remote.*)

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

// Default ceiling (bytes) on the decompressed size of a single ZIM entry the
// extension will materialize into memory (overridable via the zim_max_content_size
// setting; 0 disables the cap). A ZIM cluster is compressed (zstd/xz/lzma), so a
// small crafted archive can declare a huge uncompressed item and force an
// unbounded allocation (decompression bomb). This cap fails such reads cleanly
// before allocating. Sized generously so normal articles and typical embedded
// media read unaffected; lower it when reading untrusted archives.
static constexpr uint64_t DEFAULT_MAX_CONTENT_SIZE = 2ull * 1024 * 1024 * 1024; // 2 GiB

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
	// Ceiling (bytes) on the decompressed size of any single entry materialized
	// during this scan. 0 disables the cap. Defaults to the protective cap so a
	// caller that materializes content without explicitly setting it is still
	// guarded against decompression-bomb clusters (see zim_max_content_size).
	uint64_t max_content_bytes = DEFAULT_MAX_CONTENT_SIZE;
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

// Seekable, opaque reader over one ZIM entry's decompressed content.
//
// Exists so the zim:// VFS can serve *ranged* reads without ever holding the
// whole entry: it pins the resolved libzim item (so each read skips the dirent
// lookup) and copies out only the requested window. libzim keeps the entry's
// decompressed cluster in its own LRU cache, so successive windows over one
// entry do not re-decompress.
//
// What this does and does not buy: a consumer doing ranged reads never
// materializes the entry. A consumer that reads the whole file anyway
// (read_blob / read_text) still ends up with one full copy -- in *its* buffer,
// not an extra one of ours -- so for those the win is halving peak extension
// memory, not eliminating it.
//
// The reader does NOT keep its ZimArchive alive; hold the pool's
// shared_ptr<ZimArchive> alongside it for as long as the reader is used.
class ZimContentReader {
public:
	~ZimContentReader();
	ZimContentReader(ZimContentReader &&) noexcept;
	ZimContentReader &operator=(ZimContentReader &&) noexcept;
	ZimContentReader(const ZimContentReader &) = delete;
	ZimContentReader &operator=(const ZimContentReader &) = delete;

	// Decompressed size of the entry in bytes (resolved once, at open).
	uint64_t Size() const;

	// Copies at most `length` bytes starting at `offset` into `out`, clamped to
	// the entry's end; returns the number of bytes actually written (0 at or past
	// EOF, or when `length` is 0). Never writes more than `length` bytes, so the
	// caller's allocation is the only bound that matters -- which is why a ranged
	// read needs no separate decompression-bomb cap.
	uint64_t ReadAt(uint64_t offset, uint64_t length, char *out) const;

private:
	friend class ZimArchive;
	struct Impl;
	explicit ZimContentReader(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

// RAII handle around a single open zim::Archive. Cheap to copy by shared_ptr
// via the ArchivePool; never construct directly outside the pool in the binding
// layer (the pool keeps the libzim cluster cache warm across queries).
// Default cap for copying a remote ZIM's full-text index locally to enable search
// (overridable via the zim_remote_search_max_local_index setting). Sized so the
// first remote search stays snappy on broadband; big indexes want Phase B.
static constexpr uint64_t DEFAULT_MAX_LOCAL_SEARCH_INDEX = 8ull * 1024 * 1024; // 8 MB

class ZimArchive {
public:
	// Throws std::runtime_error (wrapping zim::ZimFileFormatError etc.) on failure.
	// `fs` is only used when `file_path` is a remote URL (s3/http/...): the archive
	// is then opened through a DuckDB FileHandle (byte-range reads) instead of the
	// local mmap path. Local files ignore `fs` and keep the mmap fast path.
	static std::shared_ptr<ZimArchive> Open(const std::string &file_path, FileSystem *fs = nullptr,
	                                        uint64_t max_local_index_bytes = DEFAULT_MAX_LOCAL_SEARCH_INDEX);
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
	// `max_content_bytes` caps the decompressed blob size when want_content is set
	// (0 disables the cap); an oversize entry throws rather than allocating.
	std::optional<ZimEntry> GetByPath(const std::string &path, bool want_content, uint64_t max_content_bytes = 0) const;
	std::optional<ZimEntry> GetByTitle(const std::string &title, bool want_content,
	                                   uint64_t max_content_bytes = 0) const;
	// Just the bytes for a content path (redirect-following). nullopt if absent.
	// `max_content_bytes` caps the decompressed size (0 disables); throws if exceeded.
	std::optional<std::string> GetContent(const std::string &path, uint64_t max_content_bytes = 0) const;
	// A ranged reader over a content path's bytes (redirect-following), for
	// consumers that must not materialize the whole entry. nullopt if absent.
	// `max_content_bytes` is checked here against the entry's *declared* size
	// (0 disables) and throws if exceeded, without allocating. Opening still
	// enforces the cap even though reads are now ranged, because the VFS's
	// dominant consumers (read_blob / read_text) read the whole entry anyway,
	// into an allocation this extension does not control; dropping the check
	// would just relocate the unbounded allocation into DuckDB. Individual
	// ReadAt calls need no cap -- they are bounded by the caller's own buffer.
	std::optional<ZimContentReader> OpenContent(const std::string &path, uint64_t max_content_bytes = 0) const;
	std::optional<std::string> GetMimetype(const std::string &path) const;
	std::optional<std::string> GetRedirectTarget(const std::string &path) const;
	std::string MainEntryPath() const; // redirect-resolved; "" if none

	// --- content scan / listing --------------------------------------------
	ZimScanCursor Scan(const ScanSpec &spec) const;

	// --- parallel scan support ---------------------------------------------
	// Total content entries — the index space a parallel scan partitions across
	// threads. Content entries are addressable by cluster-order index in
	// [0, ContentEntryCount()).
	uint64_t ContentEntryCount() const;
	// Process the content entry at cluster-order index `idx` under `spec` (the same
	// mimetype filter + content gate the cursor applies). nullopt if filtered out.
	std::optional<ZimEntry> ScanIndex(uint64_t idx, const ScanSpec &spec) const;

	// --- metadata (M namespace) --------------------------------------------
	std::vector<std::string> MetadataKeys() const;
	std::optional<std::string> Metadata(const std::string &key) const; // value bytes
	std::optional<std::string> MetadataMimetype(const std::string &key) const;
	// Parsed `Counter` metadata: mimetype -> count. Empty if no Counter key.
	std::map<std::string, int64_t> Counter() const;

	// --- full-text search (optional libzim feature) ------------------------
	bool HasFulltextIndex() const;
	// Returns hits [offset, offset+limit). Empty when no fulltext index.
	// `max_content_bytes` caps the per-hit body size read for snippet generation
	// (0 disables); a hit whose body exceeds the cap comes back without a snippet.
	std::vector<ZimSearchHit> Search(const std::string &query, uint32_t offset, uint32_t limit,
	                                 bool with_snippet = true, uint64_t max_content_bytes = 0) const;

	// --- title autocomplete ------------------------------------------------
	// Ranked title suggestions [offset, offset+limit). Backed by libzim's
	// SuggestionSearcher where xapian is present; degrades to a findByTitle
	// prefix listing otherwise (so it works on search-less builds too).
	std::vector<ZimSearchHit> Suggest(const std::string &query, uint32_t offset, uint32_t limit,
	                                  bool with_snippet = true) const;

	// --- archive-level utilities -------------------------------------------
	// Illustration (favicon / cover) PNG bytes of the requested square size;
	// nullopt when the archive has no illustration at that size.
	// `max_content_bytes` caps the decompressed illustration size (0 disables).
	std::optional<std::string> Illustration(unsigned int size, uint64_t max_content_bytes = 0) const;
	// A random content entry's path; "" when the archive has no content entries.
	std::string RandomPath() const;
	// libzim's archive integrity check (checksum + structural validation).
	bool CheckIntegrity() const;

private:
	ZimArchive(const std::string &file_path, FileSystem *fs, uint64_t max_local_index_bytes);
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
