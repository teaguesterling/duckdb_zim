//===----------------------------------------------------------------------===//
// zim_remote.hpp
//
// Bridges a DuckDB FileHandle (e.g. an httpfs S3/HTTP handle that serves byte
// ranges) to libzim's zim::IRandomAccessReader, so a ZIM archive on remote
// storage can be opened and read by fetching only the byte ranges a query
// touches -- never downloading the whole file.
//
// The libzim side of this seam (the IRandomAccessReader interface + the
// Archive(shared_ptr<IRandomAccessReader>) constructor) is carried by our vcpkg
// overlay patch (vcpkg_ports/libzim/stream-reader-api.patch) until it lands
// upstream.
//
// Concurrency: libzim issues readAt() concurrently across threads (the parallel
// read_zim scan reads cluster-order morsels in parallel). A single DuckDB
// FileHandle is not guaranteed safe for concurrent positioned reads, so readAt()
// serializes I/O under a mutex. Remote reads are network-bound and libzim's
// cluster cache absorbs most repeat reads, so this is not the bottleneck;
// decompression/parsing still parallelize per morsel.
//===----------------------------------------------------------------------===//
#pragma once

#include <zim/irandomaccessreader.h>

#include "duckdb/common/file_system.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace duckdb {
namespace zim_ext {

class DuckdbZimRemoteReader : public zim::IRandomAccessReader {
public:
	// Opens `path` for reading through `fs` (typically a remote httpfs path).
	// Throws std::runtime_error with a remediation hint if the open fails (e.g.
	// httpfs not loaded, or external access disabled).
	DuckdbZimRemoteReader(FileSystem &fs, const std::string &path);
	~DuckdbZimRemoteReader() override;

	zim::size_type getSize() const override;
	void readAt(char *dest, zim::offset_type offset, zim::size_type size) const override;

private:
	std::string path_;
	unique_ptr<FileHandle> handle_;
	zim::size_type size_ = 0;
	mutable std::mutex io_mutex_;
	// Total bytes libzim has requested via readAt(). Proves the access pattern is
	// partial: a listing scan requests far fewer bytes than getSize(). When the
	// ZIM_REMOTE_TRACE env var is set, this is reported on destruction. The atomic
	// add is negligible; the counter is otherwise unused, so there is no cost when
	// the trace is off.
	mutable std::atomic<uint64_t> bytes_requested_ {0};
};

} // namespace zim_ext
} // namespace duckdb
