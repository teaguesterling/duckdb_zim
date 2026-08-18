//===----------------------------------------------------------------------===//
// zim_archive_pool.hpp
//
// Per-DatabaseInstance cache of open ZimArchive handles keyed by canonical file
// path. Keeping a handle open keeps libzim's decompressed-cluster cache warm
// across queries (DESIGN §6), and it is a property of the POOL, not of ATTACH.
// The read_zim table function, the scalar functions, the zim:// filesystem, and
// a (future) ATTACH all pull from here, so they share one open handle and one
// warm cache per file.
//
// LIFETIME — why the pool lives in the DB's ObjectCache, not a process-static
// singleton:
//   The pool's strong-ref pin (below) keeps a remote archive's httpfs FileHandle
//   alive. A process-static singleton is destroyed at static-destruction time,
//   AFTER DuckDB has torn down its FileSystem, so closing that handle touches
//   dead state and the process hangs on exit. DuckDB's ObjectCache is the blessed
//   fix: DatabaseInstance::~DatabaseInstance() calls object_cache.reset() in the
//   destructor BODY (database.cpp), before its `config` member (which owns the
//   VirtualFileSystem, and thus httpfs) is destroyed. So entries are released
//   while the FileSystem is still alive. Scoping per-DB also stops one DB from
//   handing another DB's FileSystem-bound handle out of a shared global.
//
// Archives are held by weak_ptr; while any query holds a shared_ptr the handle
// (and its cache) stays alive. A bounded strong-ref LRU (Pin) keeps the last few
// hot regardless of in-flight queries — cheap for local mmap, valuable for remote
// (avoids a fresh httpfs connection + header/dirent re-read per call).
//
// REVALIDATION (issue #38) — a cached handle is not trusted forever:
//   Replacing a .zim in place is routine (Kiwix ships updated archives under the
//   same filename). A pool that never rechecks keeps serving the OLD bytes for
//   the life of the DB, while the zim:// VFS's GetLastModifiedTime reports the
//   NEW file's mtime — so a caching consumer (read_parquet over zim://) sees a
//   changed mtime, re-reads, and gets the same stale bytes with no way to notice.
//   Get() therefore stats the local file and reopens when its identity moved.
//   Reopening only ever REPLACES the pool's own entry; the displaced ZimArchive
//   is never mutated and stays alive until its last shared_ptr holder (an open
//   zim:// FileHandle, an in-flight scan) releases it. See zim_archive_pool.cpp
//   for the exact rule, and why remote archives are exempt.
//===----------------------------------------------------------------------===//
#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "duckdb/storage/object_cache.hpp"

#include "zim_access.hpp"

namespace duckdb {
class ClientContext;
class DatabaseInstance;
class FileSystem;

namespace zim_ext {

class ArchivePool {
public:
	ArchivePool() = default;

	// Returns an open archive for `file_path`, opening it if necessary. Throws
	// std::runtime_error if the file cannot be opened.
	//
	// `fs` is only used when `file_path` is a remote URL and the entry isn't
	// already warm: the archive is then opened through a DuckDB FileHandle
	// (byte-range reads). Local paths ignore it. Callers obtain `fs` from the same
	// source they obtained this pool (the ClientContext / DatabaseInstance).
	std::shared_ptr<ZimArchive> Get(const std::string &file_path, FileSystem *fs = nullptr,
	                                uint64_t max_local_index_bytes = DEFAULT_MAX_LOCAL_SEARCH_INDEX);

private:
	// Hold a strong reference to the most-recently-used archives so they (and their
	// libzim cluster caches) survive between calls/queries. Most-recent at the front.
	// Re-pinning a key that is already present REPLACES its archive, which is what
	// drops the pool's last strong ref to a superseded handle after a reopen.
	void Pin(const std::string &key, const std::shared_ptr<ZimArchive> &archive);

	// Identity of the file a cached archive was opened from, used to notice an
	// in-place replacement. `valid` is false for remote URLs (nothing to stat) and
	// for a local path whose stat failed; see ShouldReopen below.
	struct FileIdentity {
		bool valid = false;
		uint64_t dev = 0;
		uint64_t inode = 0;
		uint64_t size = 0;
		int64_t mtime_sec = 0;
		int64_t mtime_nsec = 0;

		bool operator==(const FileIdentity &o) const {
			return valid == o.valid && dev == o.dev && inode == o.inode && size == o.size && mtime_sec == o.mtime_sec &&
			       mtime_nsec == o.mtime_nsec;
		}
	};

	struct CacheEntry {
		std::weak_ptr<ZimArchive> archive;
		FileIdentity identity; // of the file this archive was opened from
	};

	// Stats a local path; returns an invalid identity if the stat fails. Defined in
	// the .cpp, which is where the rationale for stat-vs-FileSystem lives.
	static FileIdentity StatIdentity(const std::string &path);
	// True only on positive evidence that the file changed (both identities read
	// successfully and they differ) -- an unreadable stat is never taken as change.
	static bool ShouldReopen(const FileIdentity &cached, const FileIdentity &now);

	std::mutex mu_;
	std::unordered_map<std::string, CacheEntry> cache_;
	// Bounded LRU of strong refs. Without this, archives are weak_ptr-only and are
	// freed the instant no query holds them — so the next call reopens. Cheap for a
	// local mmap but expensive for a remote archive (new httpfs connection +
	// header/dirent re-read). Pinning the last few keeps remote handles (and their
	// keep-alive connections) and decompressed-cluster caches warm.
	static constexpr size_t MAX_PINNED = 4;
	std::list<std::pair<std::string, std::shared_ptr<ZimArchive>>> pinned_; // front = MRU
};

// ObjectCacheEntry wrapper that owns one ArchivePool, scoped to a DatabaseInstance.
// Non-evictable (GetEstimatedCacheMemory returns an invalid index): it is released
// only when the DB's ObjectCache is cleared at DatabaseInstance teardown — which
// happens before the FileSystem is destroyed, so any pinned remote handle closes
// cleanly. MAX_PINNED already bounds its memory, so it skips the buffer-pool path.
class ZimArchivePoolEntry : public ObjectCacheEntry {
public:
	ArchivePool pool;

	static std::string ObjectType() {
		return "zim_archive_pool";
	}
	std::string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // invalid => non-evictable, lives for the DB's lifetime
	}
};

// Reach the per-DatabaseInstance pool. Both overloads resolve to the same
// ObjectCache entry on the owning DatabaseInstance. Use the ClientContext form
// from table/scalar functions; the DatabaseInstance form from the zim:// VFS,
// whose FileSystem methods receive no ClientContext.
ArchivePool &GetArchivePool(ClientContext &context);
ArchivePool &GetArchivePool(DatabaseInstance &db);

} // namespace zim_ext
} // namespace duckdb
