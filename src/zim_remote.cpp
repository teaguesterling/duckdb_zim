//===----------------------------------------------------------------------===//
// zim_remote.cpp — DuckDB FileHandle -> zim::IRandomAccessReader adapter.
//===----------------------------------------------------------------------===//
#include "zim_remote.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace duckdb {
namespace zim_ext {

DuckdbZimRemoteReader::DuckdbZimRemoteReader(FileSystem &fs, const std::string &path) : path_(path) {
	try {
		handle_ = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	} catch (const std::exception &e) {
		// Most common causes: the httpfs extension isn't loaded (no filesystem
		// registered for http/s3), or enable_external_access is off. Surface a
		// hint rather than libzim's opaque open failure.
		throw std::runtime_error(std::string("failed to open remote ZIM '") + path + "': " + e.what() +
		                         " (remote archives need the httpfs extension -- INSTALL httpfs; LOAD httpfs; "
		                         "-- and enable_external_access enabled)");
	}
	if (!handle_) {
		throw std::runtime_error("failed to open remote ZIM '" + path + "'");
	}
	size_ = static_cast<zim::size_type>(handle_->GetFileSize());
}

DuckdbZimRemoteReader::~DuckdbZimRemoteReader() {
	if (std::getenv("ZIM_REMOTE_TRACE")) {
		const auto req = bytes_requested_.load(std::memory_order_relaxed);
		std::fprintf(stderr, "[zim-remote] %s: requested %llu of %llu bytes (%.1f%%)\n", path_.c_str(),
		             static_cast<unsigned long long>(req), static_cast<unsigned long long>(size_),
		             size_ ? 100.0 * static_cast<double>(req) / static_cast<double>(size_) : 0.0);
	}
}

zim::size_type DuckdbZimRemoteReader::getSize() const {
	return size_;
}

void DuckdbZimRemoteReader::readAt(char *dest, zim::offset_type offset, zim::size_type size) const {
	// FileHandle::Read(buffer, nr_bytes, location) is a positioned read that
	// reads exactly nr_bytes (throwing on short read) -- the contract libzim's
	// readAt() requires. Serialize to keep a single handle concurrency-safe.
	bytes_requested_.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
	std::lock_guard<std::mutex> guard(io_mutex_);
	handle_->Read(dest, static_cast<idx_t>(size), static_cast<idx_t>(offset));
}

} // namespace zim_ext
} // namespace duckdb
