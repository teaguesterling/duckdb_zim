//===----------------------------------------------------------------------===//
// zim_archive_pool.cpp
//===----------------------------------------------------------------------===//
#include "zim_archive_pool.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/main/database.hpp"

#include <filesystem>

namespace duckdb {
namespace zim_ext {

// Single key per DatabaseInstance: the pool holds every archive for that DB.
static constexpr const char *POOL_KEY = "zim_archive_pool";

// Cache key: an absolute, normalized path so that './x.zim', 'x.zim', and the absolute
// form all share one warm handle. weakly_canonical tolerates non-existent paths (it
// won't throw before we get a chance to surface a clean "failed to open" error).
// Remote URLs (s3://, http://, ...) are keyed verbatim -- weakly_canonical would
// mangle them as local paths.
static std::string CanonicalKey(const std::string &file_path) {
	if (FileSystem::IsRemoteFile(file_path)) {
		return file_path;
	}
	try {
		return std::filesystem::weakly_canonical(std::filesystem::absolute(file_path)).string();
	} catch (...) {
		return file_path; // any filesystem error: fall back to the raw path
	}
}

std::shared_ptr<ZimArchive> ArchivePool::Get(const std::string &file_path, FileSystem *fs) {
	const std::string key = CanonicalKey(file_path);
	std::lock_guard<std::mutex> guard(mu_);

	auto it = cache_.find(key);
	if (it != cache_.end()) {
		if (auto existing = it->second.lock()) {
			Pin(key, existing); // refresh MRU so the warm handle survives the next gap
			return existing;    // warm: reuse the open handle + cluster cache
		}
		cache_.erase(it); // expired weak_ptr
	}

	// Open with the caller's original path so the error message shows what they passed.
	auto archive = ZimArchive::Open(file_path, fs); // may throw
	cache_[key] = archive;
	Pin(key, archive);
	return archive;
}

void ArchivePool::Pin(const std::string &key, const std::shared_ptr<ZimArchive> &archive) {
	// Move-to-front if already pinned; otherwise push to front and evict the LRU
	// tail. Called under mu_.
	for (auto it = pinned_.begin(); it != pinned_.end(); ++it) {
		if (it->first == key) {
			pinned_.splice(pinned_.begin(), pinned_, it);
			return;
		}
	}
	pinned_.emplace_front(key, archive);
	while (pinned_.size() > MAX_PINNED) {
		pinned_.pop_back(); // drops the strong ref; the archive lives only as long as
		                    // an in-flight query (or a newer pin) still holds it
	}
}

ArchivePool &GetArchivePool(ClientContext &context) {
	auto entry = ObjectCache::GetObjectCache(context).GetOrCreate<ZimArchivePoolEntry>(POOL_KEY);
	return entry->pool;
}

ArchivePool &GetArchivePool(DatabaseInstance &db) {
	auto entry = db.GetObjectCache().GetOrCreate<ZimArchivePoolEntry>(POOL_KEY);
	return entry->pool;
}

} // namespace zim_ext
} // namespace duckdb
