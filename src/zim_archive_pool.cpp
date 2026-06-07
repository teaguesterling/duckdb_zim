//===----------------------------------------------------------------------===//
// zim_archive_pool.cpp
//===----------------------------------------------------------------------===//
#include "zim_archive_pool.hpp"

namespace duckdb {
namespace zim_ext {

ArchivePool &ArchivePool::Instance() {
	static ArchivePool instance;
	return instance;
}

std::shared_ptr<ZimArchive> ArchivePool::Get(const std::string &file_path) {
	std::lock_guard<std::mutex> guard(mu_);

	auto it = cache_.find(file_path);
	if (it != cache_.end()) {
		if (auto existing = it->second.lock()) {
			return existing; // warm: reuse the open handle + cluster cache
		}
		cache_.erase(it); // expired weak_ptr
	}

	auto archive = ZimArchive::Open(file_path); // may throw
	cache_[file_path] = archive;
	return archive;
}

void ArchivePool::Evict(const std::string &file_path) {
	std::lock_guard<std::mutex> guard(mu_);
	cache_.erase(file_path);
}

void ArchivePool::Clear() {
	std::lock_guard<std::mutex> guard(mu_);
	cache_.clear();
}

} // namespace zim_ext
} // namespace duckdb
