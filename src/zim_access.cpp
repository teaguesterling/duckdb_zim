//===----------------------------------------------------------------------===//
// zim_access.cpp — implementation of the libzim access layer.
//
// This is the only translation unit (with zim_archive_pool.cpp) that includes
// <zim/*>. Semantics encoded here are verified in docs/libzim-semantics.md.
//
// Lines marked `// VERIFY:` use a libzim C++ signature that matched the python
// binding's behavior but should be confirmed to compile against the pinned
// libzim version; the behavior is settled, only the exact spelling may drift.
//===----------------------------------------------------------------------===//
#include "zim_access.hpp"
#include "zim_remote.hpp" // DuckdbZimRemoteReader + FileSystem (remote opens)

#include <cstring> // std::memcpy (ZimContentReader::ReadAt)
#include <stdexcept>
#include <utility> // std::declval, for the EntryRange iterator type alias

#include <zim/archive.h>
#include <zim/entry.h>
#include <zim/item.h>
#include <zim/blob.h>
#include <zim/error.h>
#include <zim/uuid.h>
// zim/search.h (and the Searcher API) only exist when libzim is built with xapian.
// LIBZIM_WITH_XAPIAN comes from zim/zim_config.h, pulled in transitively via zim/archive.h.
#ifdef LIBZIM_WITH_XAPIAN
#include <zim/search.h>
#include <zim/suggestion.h>
#endif

namespace duckdb {
namespace zim_ext {

//===--------------------------------------------------------------------===//
// helpers
//===--------------------------------------------------------------------===//

std::string NormalizeContentPath(const std::string &path) {
	// The high-level API tolerates an optional leading "C/" on content paths
	// and strips it on readback (oracle Q1/Q5). Mirror that here so callers may
	// pass either form. Only strip a real "C/" prefix, not e.g. "Calcium".
	if (path.size() >= 2 && path[0] == 'C' && path[1] == '/') {
		return path.substr(2);
	}
	return path;
}

static std::string BlobToString(const zim::Blob &blob) {
	return std::string(blob.data(), blob.size());
}

// The cheap half of the decompression-bomb guard: a ZIM cluster is compressed, so
// a crafted archive can declare a huge uncompressed item size and force an
// unbounded allocation. Reject on the item's *declared* size, before anything is
// allocated. `max_bytes == 0` disables the cap; `what` names the entry for the
// error message. Shared by the materializing paths below and by OpenContent,
// which allocates nothing itself but still refuses to hand out a reader for an
// entry its callers would go on to materialize.
static void CheckDeclaredSizeCap(uint64_t declared, uint64_t max_bytes, const std::string &what) {
	if (max_bytes != 0 && declared > max_bytes) {
		throw std::runtime_error("zim: " + what + " decompressed size " + std::to_string(declared) +
		                         " bytes exceeds zim_max_content_size (" + std::to_string(max_bytes) +
		                         " bytes); raise zim_max_content_size or set it to 0 to disable the cap");
	}
}

// Shared, capped materialization for every whole-entry getData() site. The blob
// is re-checked after the fact as defense-in-depth, in case a declared size ever
// understates the real output.
static std::string MaterializeCapped(const zim::Item &item, uint64_t max_bytes, const std::string &what) {
	CheckDeclaredSizeCap(static_cast<uint64_t>(item.getSize()), max_bytes, what);
	auto blob = item.getData();
	if (max_bytes != 0 && static_cast<uint64_t>(blob.size()) > max_bytes) {
		throw std::runtime_error("zim: " + what + " materialized size " + std::to_string(blob.size()) +
		                         " bytes exceeds zim_max_content_size (" + std::to_string(max_bytes) +
		                         " bytes); the archive's declared item size understated the real output");
	}
	return BlobToString(blob);
}

// Materialize the non-content metadata for an entry (no blob fetch).
static ZimEntry EntryMeta(const zim::Entry &entry) {
	ZimEntry row;
	row.path = entry.getPath();
	row.title = entry.getTitle();
	row.is_redirect = entry.isRedirect();
	if (row.is_redirect) {
		row.redirect_path = entry.getRedirectEntry().getPath();
	} else {
		auto item = entry.getItem(false);
		row.mimetype = item.getMimetype();
		row.size = item.getSize();
	}
	return row;
}

// One Accept-style pattern vs a concrete mimetype. Redirects (empty mimetype)
// never match a pattern.
static bool MimetypePatternMatch(const std::string &pattern, const std::string &mimetype) {
	if (mimetype.empty()) {
		return false;
	}
	if (pattern == "*" || pattern == "*/*") {
		return true;
	}
	if (pattern == mimetype) {
		return true;
	}
	// "type/*": match on the major type prefix.
	if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, "/*") == 0) {
		const std::string prefix = pattern.substr(0, pattern.size() - 1); // keep the slash, e.g. "image/"
		return mimetype.rfind(prefix, 0) == 0;
	}
	return false;
}

bool MimetypeAccepted(const std::vector<std::string> &patterns, const std::string &mimetype) {
	if (patterns.empty()) {
		return true;
	}
	for (const auto &p : patterns) {
		if (MimetypePatternMatch(p, mimetype)) {
			return true;
		}
	}
	return false;
}

static void LoadContent(const zim::Entry &entry, ZimEntry &row, uint64_t max_content_bytes) {
	if (row.is_redirect) {
		row.content_loaded = true; // redirects have no body
		return;
	}
	auto item = entry.getItem(true); // follow redirects
	row.content = MaterializeCapped(item, max_content_bytes, "entry '" + row.path + "'");
	row.content_loaded = true;
}

//===--------------------------------------------------------------------===//
// ZimScanCursor
//===--------------------------------------------------------------------===//

// We hold the begin/end iterators of whichever EntryRange the ScanSpec selected.
// zim::Archive::EntryRange<order>::iterator is the iterator type; iterByPath /
// findByPath return ORDER_PATH ranges, iterByTitle / findByTitle ORDER_TITLE.
// Both iterator types are stored type-erased behind a small virtual stepper so
// the cursor has one shape regardless of order.
struct CursorStepper {
	virtual ~CursorStepper() = default;
	virtual bool AtEnd() const = 0;
	virtual zim::Entry Current() const = 0;
	virtual void Advance() = 0;
};

template <typename RangeT>
struct RangeStepper : CursorStepper {
	// libzim's Archive::EntryRange<order> has no nested `iterator` typedef; its
	// begin()/end() return a separate zim::Archive::iterator<order>. Derive that
	// type from begin() (which is const-qualified) instead of RangeT::iterator.
	using IterT = decltype(std::declval<const RangeT &>().begin());
	RangeT range;
	IterT it;
	IterT end;
	explicit RangeStepper(RangeT r) : range(std::move(r)), it(range.begin()), end(range.end()) {
	}
	bool AtEnd() const override {
		return it == end;
	}
	zim::Entry Current() const override {
		return *it;
	}
	void Advance() override {
		++it;
	}
};

struct ZimScanCursor::Impl {
	std::unique_ptr<CursorStepper> stepper;
	ScanSpec spec;
};

ZimScanCursor::ZimScanCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
ZimScanCursor::~ZimScanCursor() = default;
ZimScanCursor::ZimScanCursor(ZimScanCursor &&) noexcept = default;
ZimScanCursor &ZimScanCursor::operator=(ZimScanCursor &&) noexcept = default;

bool ZimScanCursor::Next(ZimEntry &out) {
	auto &spec = impl_->spec;
	auto &step = *impl_->stepper;
	while (!step.AtEnd()) {
		zim::Entry entry = step.Current();
		step.Advance();

		ZimEntry row = EntryMeta(entry);

		// Accept-style mimetype post-filter (libzim has no mimetype index).
		// Redirects have no mimetype, so a non-empty filter excludes them.
		if (!spec.mimetype_filter.empty() && !MimetypeAccepted(spec.mimetype_filter, row.mimetype)) {
			continue;
		}
		if (spec.want_content && MimetypeAccepted(spec.content_mimetypes, row.mimetype)) {
			LoadContent(entry, row, spec.max_content_bytes);
		}
		out = std::move(row);
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// ZimArchive
//===--------------------------------------------------------------------===//

ZimArchive::ZimArchive(const std::string &file_path, FileSystem *fs, uint64_t max_local_index_bytes)
    : file_path_(file_path) {
	try {
		if (fs && FileSystem::IsRemoteFile(file_path)) {
			// Remote archive: read byte ranges through a DuckDB FileHandle. The
			// zim::Archive holds the reader alive (StreamFileReader keeps a
			// shared_ptr to it), so we don't store it separately here.
			auto reader = std::make_shared<DuckdbZimRemoteReader>(*fs, file_path);
			// Remote full-text search: when the (uncompressed) Xapian index is <= the cap
			// (zim_remote_search_max_local_index), libzim copies just the index blob out
			// through the reader to a temp file and opens Xapian on it. preloadXapianDb is
			// OFF so the index is fetched lazily on the first search, not at open: a
			// non-search remote query (read_zim/metadata) pays nothing for the index.
			// NOTE: preloadXapianDb is left ON (default). Lazy load (preloadXapianDb(false))
			// would avoid fetching the index for non-search remote queries, but libzim's
			// lazy getXapianDb path returns null for reader-backed archives (search then
			// throws "Cannot create Search without FT Xapian index"). Revisit as an
			// optimization once that libzim path is understood. The index is small
			// (<= the cap) so eager preload is a bounded cost.
			archive_ = std::make_unique<zim::Archive>(
			    reader, zim::OpenConfig().maxLocalSearchIndexBytes(max_local_index_bytes));
		} else {
			// Local archive: libzim opens the file directly (mmap fast path).
			archive_ = std::make_unique<zim::Archive>(file_path);
		}
	} catch (const std::exception &e) {
		throw std::runtime_error("failed to open ZIM '" + file_path + "': " + e.what());
	}
}

ZimArchive::~ZimArchive() = default;

std::shared_ptr<ZimArchive> ZimArchive::Open(const std::string &file_path, FileSystem *fs,
                                             uint64_t max_local_index_bytes) {
	// Cannot std::make_shared with a private ctor; wrap explicitly.
	return std::shared_ptr<ZimArchive>(new ZimArchive(file_path, fs, max_local_index_bytes));
}

ZimInfo ZimArchive::Info() const {
	ZimInfo info;
	info.uuid = std::string(archive_->getUuid()); // VERIFY: Uuid has operator std::string
	info.entry_count = archive_->getEntryCount();
	info.all_entry_count = archive_->getAllEntryCount();
	info.article_count = archive_->getArticleCount();
	info.media_count = archive_->getMediaCount();
	info.main_entry_path = MainEntryPath();
	info.has_fulltext_index = archive_->hasFulltextIndex();
	info.has_title_index = archive_->hasTitleIndex();
	info.has_checksum = archive_->hasChecksum();
	info.is_multipart = archive_->isMultiPart();
	info.has_new_namespace_scheme = archive_->hasNewNamespaceScheme();
	info.filesize = archive_->getFilesize();
	return info;
}

bool ZimArchive::HasEntry(const std::string &path) const {
	return archive_->hasEntryByPath(NormalizeContentPath(path));
}

std::optional<ZimEntry> ZimArchive::GetByPath(const std::string &path, bool want_content,
                                              uint64_t max_content_bytes) const {
	auto p = NormalizeContentPath(path);
	if (!archive_->hasEntryByPath(p)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByPath(p);
	ZimEntry row = EntryMeta(entry);
	if (want_content) {
		LoadContent(entry, row, max_content_bytes);
	}
	return row;
}

std::optional<ZimEntry> ZimArchive::GetByTitle(const std::string &title, bool want_content,
                                               uint64_t max_content_bytes) const {
	if (!archive_->hasEntryByTitle(title)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByTitle(title);
	ZimEntry row = EntryMeta(entry);
	if (want_content) {
		LoadContent(entry, row, max_content_bytes);
	}
	return row;
}

std::optional<std::string> ZimArchive::GetContent(const std::string &path, uint64_t max_content_bytes) const {
	auto p = NormalizeContentPath(path);
	if (!archive_->hasEntryByPath(p)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByPath(p);
	auto item = entry.getItem(true); // follow redirects
	return MaterializeCapped(item, max_content_bytes, "entry '" + p + "'");
}

//===--------------------------------------------------------------------===//
// ZimContentReader
//===--------------------------------------------------------------------===//

// Holds the resolved (redirect-followed) libzim item plus its decompressed size.
// Keeping the zim::Item alive is what makes a ranged read cheap: the dirent
// lookup and redirect resolution happen once, at open, and every ReadAt after
// that is a cluster-cache hit plus a memcpy of the requested window.
struct ZimContentReader::Impl {
	zim::Item item;
	uint64_t size;

	Impl(zim::Item item_p, uint64_t size_p) : item(std::move(item_p)), size(size_p) {
	}
};

ZimContentReader::ZimContentReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
ZimContentReader::~ZimContentReader() = default;
ZimContentReader::ZimContentReader(ZimContentReader &&) noexcept = default;
ZimContentReader &ZimContentReader::operator=(ZimContentReader &&) noexcept = default;

uint64_t ZimContentReader::Size() const {
	return impl_->size;
}

uint64_t ZimContentReader::ReadAt(uint64_t offset, uint64_t length, char *out) const {
	if (length == 0 || offset >= impl_->size) {
		return 0; // zero-length request, or at/past EOF
	}
	// Clamp to the entry's end ourselves rather than trusting libzim's clamp: the
	// only thing standing between a malformed archive and a buffer overrun here is
	// that we never copy more bytes than the caller asked for.
	const uint64_t available = impl_->size - offset;
	const uint64_t want = length < available ? length : available;
	auto blob = impl_->item.getData(static_cast<zim::offset_type>(offset), static_cast<zim::size_type>(want));
	uint64_t got = static_cast<uint64_t>(blob.size());
	if (got > want) {
		got = want; // defensive: never write past the caller's `length` bytes
	}
	if (got > 0) {
		std::memcpy(out, blob.data(), static_cast<size_t>(got));
	}
	return got;
}

std::optional<ZimContentReader> ZimArchive::OpenContent(const std::string &path, uint64_t max_content_bytes) const {
	auto p = NormalizeContentPath(path);
	if (!archive_->hasEntryByPath(p)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByPath(p);
	auto item = entry.getItem(true); // follow redirects, like GetContent
	const uint64_t size = static_cast<uint64_t>(item.getSize());
	CheckDeclaredSizeCap(size, max_content_bytes, "entry '" + p + "'");
	return ZimContentReader(std::unique_ptr<ZimContentReader::Impl>(new ZimContentReader::Impl(std::move(item), size)));
}

std::optional<std::string> ZimArchive::GetMimetype(const std::string &path) const {
	auto p = NormalizeContentPath(path);
	if (!archive_->hasEntryByPath(p)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByPath(p);
	if (entry.isRedirect()) {
		return entry.getRedirectEntry().getItem(false).getMimetype();
	}
	return entry.getItem(false).getMimetype();
}

std::optional<std::string> ZimArchive::GetRedirectTarget(const std::string &path) const {
	auto p = NormalizeContentPath(path);
	if (!archive_->hasEntryByPath(p)) {
		return std::nullopt;
	}
	zim::Entry entry = archive_->getEntryByPath(p);
	if (!entry.isRedirect()) {
		return std::nullopt;
	}
	return entry.getRedirectEntry().getPath();
}

std::string ZimArchive::MainEntryPath() const {
	if (!archive_->hasMainEntry()) {
		return std::string();
	}
	zim::Entry main = archive_->getMainEntry();
	// Oracle Q6: the main entry is itself a redirect (mainPage -> A/...). Resolve.
	if (main.isRedirect()) {
		return main.getRedirectEntry().getPath();
	}
	return main.getPath();
}

ZimScanCursor ZimArchive::Scan(const ScanSpec &spec) const {
	auto impl = std::make_unique<ZimScanCursor::Impl>();
	impl->spec = spec;

	// Select the libzim range that matches order + prefix. findByPath/findByTitle
	// give an efficient prefix range; iterByPath/iterByTitle give the full set.
	if (spec.order == ScanOrder::ByTitle) {
		if (spec.title_prefix.has_value()) {
			auto range = archive_->findByTitle(*spec.title_prefix); // VERIFY signature
			impl->stepper = std::make_unique<RangeStepper<decltype(range)>>(std::move(range));
		} else {
			auto range = archive_->iterByTitle();
			impl->stepper = std::make_unique<RangeStepper<decltype(range)>>(std::move(range));
		}
	} else {
		if (spec.path_prefix.has_value()) {
			auto range = archive_->findByPath(NormalizeContentPath(*spec.path_prefix));
			impl->stepper = std::make_unique<RangeStepper<decltype(range)>>(std::move(range));
		} else {
			auto range = archive_->iterByPath();
			impl->stepper = std::make_unique<RangeStepper<decltype(range)>>(std::move(range));
		}
	}
	return ZimScanCursor(std::move(impl));
}

uint64_t ZimArchive::ContentEntryCount() const {
	return archive_->getEntryCount();
}

std::optional<ZimEntry> ZimArchive::ScanIndex(uint64_t idx, const ScanSpec &spec) const {
	// Cluster-order random access: a contiguous index range stays inside the same
	// clusters, so a parallel morsel decompresses each cluster once. Mirrors the
	// per-entry logic of ZimScanCursor::Next (mimetype filter + content gate).
	zim::Entry entry = archive_->getEntryByClusterOrder(static_cast<zim::entry_index_type>(idx));
	ZimEntry row = EntryMeta(entry);
	if (!spec.mimetype_filter.empty() && !MimetypeAccepted(spec.mimetype_filter, row.mimetype)) {
		return std::nullopt;
	}
	if (spec.want_content && MimetypeAccepted(spec.content_mimetypes, row.mimetype)) {
		LoadContent(entry, row, spec.max_content_bytes);
	}
	return row;
}

std::vector<std::string> ZimArchive::MetadataKeys() const {
	return archive_->getMetadataKeys();
}

std::optional<std::string> ZimArchive::Metadata(const std::string &key) const {
	auto keys = archive_->getMetadataKeys();
	for (const auto &k : keys) {
		if (k == key) {
			return archive_->getMetadata(key); // returns value bytes
		}
	}
	return std::nullopt;
}

std::optional<std::string> ZimArchive::MetadataMimetype(const std::string &key) const {
	auto keys = archive_->getMetadataKeys();
	for (const auto &k : keys) {
		if (k == key) {
			return archive_->getMetadataItem(key).getMimetype();
		}
	}
	return std::nullopt;
}

std::map<std::string, int64_t> ZimArchive::Counter() const {
	std::map<std::string, int64_t> out;
	auto raw = Metadata("Counter");
	if (!raw.has_value()) {
		return out;
	}
	// Format (oracle Q4): "image/png=1;text/css=1;text/html=3"
	const std::string &s = *raw;
	size_t i = 0;
	while (i < s.size()) {
		size_t semi = s.find(';', i);
		std::string pair = s.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
		size_t eq = pair.rfind('=');
		if (eq != std::string::npos) {
			std::string mime = pair.substr(0, eq);
			std::string num = pair.substr(eq + 1);
			try {
				out[mime] = static_cast<int64_t>(std::stoll(num));
			} catch (...) {
				// skip malformed segment
			}
		}
		if (semi == std::string::npos) {
			break;
		}
		i = semi + 1;
	}
	return out;
}

bool ZimArchive::HasFulltextIndex() const {
	return archive_->hasFulltextIndex();
}

std::vector<ZimSearchHit> ZimArchive::Search(const std::string &query, uint32_t offset, uint32_t limit,
                                             bool with_snippet, uint64_t max_content_bytes) const {
	std::vector<ZimSearchHit> hits;
#ifdef LIBZIM_WITH_XAPIAN
	if (!archive_->hasFulltextIndex()) {
		return hits; // caller decides whether to fall back to title search
	}
	// hasFulltextIndex() reports index *existence*; opening it can still fail when a
	// remote index exceeds zim_remote_search_max_local_index (not copied locally).
	// Catch that and surface an actionable error rather than returning silent empties
	// (which would read as "no matches").
	try {
		zim::Searcher searcher(*archive_);
		zim::Query q;
		q.setQuery(query);
		auto search = searcher.search(q);
		auto results = search.getResults(offset, limit);
		for (auto it = results.begin(); it != results.end(); ++it) {
			ZimSearchHit hit;
			hit.path = it.getPath();
			hit.title = it.getTitle();
			hit.score = static_cast<double>(it.getScore());
			// Snippet generation reads each hit's full body (getData, inside libzim) to
			// highlight it; skip it when the caller only wants ranked path/title -- much
			// cheaper for remote archives (no content clusters fetched, just the index +
			// dirents). Also skip it when the hit's body exceeds the output cap: a
			// decompression-bomb entry that ranks in a search must not be materialized to
			// build a snippet. Best-effort — a lookup failure falls back to the old
			// behavior (libzim already bounds a snippet's own length).
			if (with_snippet) {
				bool over_cap = false;
				if (max_content_bytes != 0) {
					try {
						auto hit_item = archive_->getEntryByPath(hit.path).getItem(true);
						over_cap = static_cast<uint64_t>(hit_item.getSize()) > max_content_bytes;
					} catch (...) {
						over_cap = false;
					}
				}
				if (!over_cap) {
					auto snippet = it.getSnippet();
					if (!snippet.empty()) {
						hit.snippet = std::move(snippet);
					}
				}
			}
			hits.push_back(std::move(hit));
		}
	} catch (const std::exception &e) {
		// The index exists (hasFulltextIndex) but the Searcher couldn't open it. Remote
		// archives range-read the index in place, so an index larger than
		// zim_remote_search_max_local_index is NOT a failure anymore. A remaining
		// "Cannot create Search" here means the index couldn't be opened at all (e.g. a
		// compressed index cluster, a corrupt index, or a remote read error); surface
		// the underlying message rather than stale copy-cap advice.
		const std::string what = e.what();
		if (what.find("Cannot create Search") != std::string::npos) {
			throw std::runtime_error(std::string("zim_search: the full-text index for '") + file_path_ +
			                         "' exists but could not be opened (it may be compressed, corrupt, or "
			                         "unreadable over the network); underlying error: " +
			                         what);
		}
		throw;
	}
#else
	// libzim built without xapian: no full-text search available. (void the args.)
	(void)query;
	(void)offset;
	(void)limit;
	(void)with_snippet;
#endif
	return hits;
}

std::vector<ZimSearchHit> ZimArchive::Suggest(const std::string &query, uint32_t offset, uint32_t limit,
                                              bool with_snippet) const {
	std::vector<ZimSearchHit> hits;
#ifdef LIBZIM_WITH_XAPIAN
	zim::SuggestionSearcher searcher(*archive_);
	auto search = searcher.suggest(query);
	auto results = search.getResults(static_cast<int>(offset), static_cast<int>(limit));
	for (auto it = results.begin(); it != results.end(); ++it) {
		ZimSearchHit hit;
		hit.path = it->getPath();
		hit.title = it->getTitle();
		if (with_snippet && it->hasSnippet()) {
			hit.snippet = it->getSnippet();
		}
		hits.push_back(std::move(hit));
	}
#else
	// No xapian: fall back to a title-prefix listing (no ranking/snippet).
	(void)with_snippet;
	uint32_t skipped = 0;
	for (auto entry : archive_->findByTitle(query)) {
		if (skipped < offset) {
			skipped++;
			continue;
		}
		if (hits.size() >= limit) {
			break;
		}
		ZimSearchHit hit;
		hit.path = entry.getPath();
		hit.title = entry.getTitle();
		hits.push_back(std::move(hit));
	}
#endif
	return hits;
}

std::optional<std::string> ZimArchive::Illustration(unsigned int size, uint64_t max_content_bytes) const {
	if (!archive_->hasIllustration(size)) {
		return std::nullopt;
	}
	auto item = archive_->getIllustrationItem(size);
	return MaterializeCapped(item, max_content_bytes, "illustration (" + std::to_string(size) + "px)");
}

std::string ZimArchive::RandomPath() const {
	try {
		return archive_->getRandomEntry().getPath();
	} catch (...) {
		return ""; // no content entries to choose from
	}
}

bool ZimArchive::CheckIntegrity() const {
	return archive_->check();
}

} // namespace zim_ext
} // namespace duckdb
