//===----------------------------------------------------------------------===//
// zim_archive_pool.cpp
//===----------------------------------------------------------------------===//
#include "zim_archive_pool.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/main/database.hpp"

#include <filesystem>
#include <sys/stat.h>

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

// --- file identity (issue #38) ------------------------------------------------
//
// Stats the local file behind a cached archive so an in-place replacement is
// noticed. (dev, inode) catches a rename-into-place -- which is exactly how a
// new archive normally arrives, libzim's own Creator included: it builds
// "<target>.tmp" and renames it over the target, so the inode always moves.
// (mtime, size) catches a write-in-place, where the inode does not.
//
// Raw ::stat rather than DuckDB's FileSystem: FileSystem exposes size and mtime
// only through an OPEN FileHandle, so using it would put an open()/close() pair
// on a path that runs once per scalar-function row, and it would still not
// surface dev/inode. This is a plain metadata read of a local path -- no libzim,
// no VFS. Windows is not a build target for this extension (the distribution
// pipeline excludes every windows_* arch), so only POSIX spellings are needed --
// but they are NOT uniform across the POSIX targets. Linux and emscripten expose
// the POSIX-2008 `st_mtim`; Apple's <sys/stat.h> names the same field
// `st_mtimespec` and does not define `st_mtim` by default, so this must be
// conditional or the macOS build fails to compile.
ArchivePool::FileIdentity ArchivePool::StatIdentity(const std::string &path) {
	FileIdentity id;
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) {
		return id; // stays invalid: see ShouldReopen for what that means
	}
	id.valid = true;
	id.dev = static_cast<uint64_t>(st.st_dev);
	id.inode = static_cast<uint64_t>(st.st_ino);
	id.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
	id.mtime_sec = static_cast<int64_t>(st.st_mtimespec.tv_sec);
	id.mtime_nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#else
	id.mtime_sec = static_cast<int64_t>(st.st_mtim.tv_sec);
	id.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
	return id;
}

// Reopen ONLY on positive evidence that the file changed: both identities were
// read successfully and they differ.
//
// The conservative direction matters. `now.valid == false` means the stat itself
// failed -- an unlinked file, a permissions change, an NFS hiccup -- which is not
// evidence that the bytes changed, and reopening would turn a transient stat
// failure into a hard query error where today the warm (mmap-backed, still
// perfectly readable) handle just keeps working. `cached.valid == false` means
// the entry was recorded without a baseline (a stat that failed at open time),
// so there is nothing to compare against; the caller records the fresh identity
// as the baseline instead of throwing the handle away.
bool ArchivePool::ShouldReopen(const FileIdentity &cached, const FileIdentity &now) {
	return cached.valid && now.valid && !(cached == now);
}

std::shared_ptr<ZimArchive> ArchivePool::Get(const std::string &file_path, FileSystem *fs,
                                             uint64_t max_local_index_bytes) {
	// The remote-index cap (zim_remote_search_max_local_index) is applied at open time
	// (OpenConfig::maxLocalSearchIndexBytes) and decides copy-locally vs range-read-in-
	// place for a remote full-text index. A handle opened under one cap keeps that cap
	// for its lifetime, so reusing it for a query that asked for a different cap would
	// silently ignore the new setting (issue #16). Fold the cap into the cache key for
	// remote archives so each distinct cap gets its own handle. Local archives ignore
	// the cap entirely, so their key stays path-only (no redundant handles).
	const bool is_remote = FileSystem::IsRemoteFile(file_path);
	std::string key = CanonicalKey(file_path);
	if (is_remote) {
		// 0x1f (unit separator) can't occur in a path or URL, so it cleanly
		// delimits the path from the cap suffix.
		key += '\x1f';
		key += "idx=";
		key += std::to_string(max_local_index_bytes);
	}

	// Revalidation (issue #38): read the file's identity BEFORE taking the lock, so
	// the stat syscall is not serialized across threads. Reading it before the open
	// below is deliberate and safe: the recorded identity can only ever be as old as
	// the archive it is stored with, never newer, so a replacement that lands in that
	// window shows up as a difference on the NEXT Get and is reopened then. The worst
	// case is one redundant reopen, never a permanently stale handle.
	//
	// Remote archives are exempt. There is no stat over HTTP byte ranges -- the only
	// equivalent is a HEAD request, i.e. a network round trip on a path that runs once
	// per scalar-function row, and a false positive there does not just reopen a mmap
	// but re-fetches the archive header, dirents and possibly the full-text index. The
	// cost/benefit is the opposite of the local case, so a remote handle keeps the
	// pre-#38 behaviour: cached for the lifetime of the DB instance.
	const FileIdentity identity = is_remote ? FileIdentity() : StatIdentity(key);

	std::lock_guard<std::mutex> guard(mu_);

	auto it = cache_.find(key);
	if (it != cache_.end()) {
		if (auto existing = it->second.archive.lock()) {
			if (!ShouldReopen(it->second.identity, identity)) {
				// Adopt a baseline if the entry never had one (its open-time stat failed).
				if (!it->second.identity.valid && identity.valid) {
					it->second.identity = identity;
				}
				Pin(key, existing); // refresh MRU so the warm handle survives the next gap
				return existing;    // warm: reuse the open handle + cluster cache
			}
			// The file was replaced. Fall through and open the new one. `existing` is
			// NOT touched: the displaced ZimArchive is immutable here, and whoever still
			// holds a shared_ptr to it (an open zim:// FileHandle and its pinned libzim
			// item, an in-flight scan cursor) keeps reading the old bytes safely until
			// they let go. Only the pool's own entry is replaced.
		}
		cache_.erase(it); // expired weak_ptr, or a superseded archive
	}

	// Open with the caller's original path so the error message shows what they passed.
	// The cap is part of the key for remote archives (above), so a handle is never
	// reused under a cap it wasn't opened with. Since Phase B (range-reading the index
	// in place) a large remote index is still searchable at a low cap -- the cap only
	// affects copy-vs-range-read (a performance choice), never correctness.
	auto archive = ZimArchive::Open(file_path, fs, max_local_index_bytes); // may throw
	cache_[key] = CacheEntry {archive, identity};
	Pin(key, archive);
	return archive;
}

void ArchivePool::Pin(const std::string &key, const std::shared_ptr<ZimArchive> &archive) {
	// Move-to-front if already pinned; otherwise push to front and evict the LRU
	// tail. Called under mu_.
	for (auto it = pinned_.begin(); it != pinned_.end(); ++it) {
		if (it->first == key) {
			// Assign, don't just splice: after a revalidation reopen the key is already
			// pinned to the SUPERSEDED archive, and leaving it there would both keep the
			// stale handle alive for no reason and (worse) leave the pool's strong ref
			// pointing at bytes nothing should be served from again.
			it->second = archive;
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
