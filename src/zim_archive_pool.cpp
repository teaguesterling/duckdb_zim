//===----------------------------------------------------------------------===//
// zim_archive_pool.cpp
//===----------------------------------------------------------------------===//
#include "zim_archive_pool.hpp"

#include "duckdb/common/file_system.hpp"

#include <filesystem>

namespace duckdb {
namespace zim_ext {

ArchivePool &ArchivePool::Instance() {
	static ArchivePool instance;
	return instance;
}

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

void ArchivePool::SetDefaultFileSystem(FileSystem *fs) {
	std::lock_guard<std::mutex> guard(mu_);
	default_fs_ = fs;
}

std::shared_ptr<ZimArchive> ArchivePool::Get(const std::string &file_path, FileSystem *fs) {
	const std::string key = CanonicalKey(file_path);
	std::lock_guard<std::mutex> guard(mu_);

	auto it = cache_.find(key);
	if (it != cache_.end()) {
		if (auto existing = it->second.lock()) {
			return existing; // warm: reuse the open handle + cluster cache
		}
		cache_.erase(it); // expired weak_ptr
	}

	// An explicit per-query FileSystem wins; otherwise fall back to the one
	// registered at extension load so context-free callers still reach remote files.
	FileSystem *use_fs = fs ? fs : default_fs_;

	// Open with the caller's original path so the error message shows what they passed.
	auto archive = ZimArchive::Open(file_path, use_fs); // may throw
	cache_[key] = archive;
	return archive;
}

void ArchivePool::Evict(const std::string &file_path) {
	std::lock_guard<std::mutex> guard(mu_);
	cache_.erase(CanonicalKey(file_path));
}

void ArchivePool::Clear() {
	std::lock_guard<std::mutex> guard(mu_);
	cache_.clear();
}

} // namespace zim_ext
} // namespace duckdb
