//===----------------------------------------------------------------------===//
// zim_archive_pool.hpp
//
// Process-wide cache of open ZimArchive handles keyed by canonical file path.
// This is the design's load-bearing decision (DESIGN §6): keeping a handle open
// keeps libzim's decompressed-cluster cache warm across queries, and it is a
// property of the POOL, not of ATTACH. The read_zim table function, the scalar
// functions, the (future) zim:// filesystem, and a (future) ATTACH all pull
// from here, so they share one open handle and one warm cache per file.
//
// Archives are held by weak_ptr: while any query holds a shared_ptr the handle
// (and its cache) stays alive; once all consumers drop it, libzim frees it.
// An optional strong-ref pin can be added later if we want to keep N archives
// hot regardless of in-flight queries.
//===----------------------------------------------------------------------===//
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "zim_access.hpp"

namespace duckdb {
namespace zim_ext {

class ArchivePool {
public:
	static ArchivePool &Instance();

	// Returns an open archive for `file_path`, opening it if necessary. Throws
	// std::runtime_error if the file cannot be opened. Path is canonicalized by
	// the caller's filesystem semantics before keying (the binding layer passes
	// the already-resolved path).
	std::shared_ptr<ZimArchive> Get(const std::string &file_path);

	// Drop the cache entry for a path (e.g. on DETACH or explicit invalidation).
	void Evict(const std::string &file_path);
	void Clear();

private:
	ArchivePool() = default;
	std::mutex mu_;
	std::unordered_map<std::string, std::weak_ptr<ZimArchive>> cache_;
};

} // namespace zim_ext
} // namespace duckdb
