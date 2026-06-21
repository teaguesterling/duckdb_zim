# Remote full-text search — implementation plan (local-copy path first)

Companion to `remote-search-design.md` (the profiling + the two-path strategy).
This is the concrete, code-grounded build order. **Status: plan / not implemented.**

Phasing follows the design's "implement the local-copy path first" guidance: the
small-index local copy needs no Xapian patch and unblocks remote `zim_search` for
the common case; the random-access glass reader is the larger follow-on for big
indexes.

## Foundation already in place (v0.3)

- `vcpkg_ports/libzim/stream-reader-api.patch` adds the public
  `zim::IRandomAccessReader` (`getSize`/`readAt`) + `Archive(shared_ptr<IRandomAccessReader>,
  OpenConfig)` ctor + internal `StreamFileReader : BaseFileReader`. It also guards the
  6 `zimFile`-coupled spots in `fileimpl.cpp`, including `getDirectAccessInformation`
  (returns invalid when reader-backed → today `loadXapianDb` → nullptr → search yields nothing).
- `src/zim_remote.cpp` — `DuckdbZimRemoteReader : zim::IRandomAccessReader` over a DuckDB
  httpfs `FileHandle` (positioned `readAt`). This is the reader the index path reuses.

## The blocker (precise)

`FileImpl::loadXapianDb()` (fileimpl.cpp) → `getDirectAccessInformation()` →
`getDbFromAccessInfo()` (tools.cpp) = `openFile(filename)` + `seek(offset)` +
`Xapian::Database(fd)`. It needs a real local fd into the ZIM file; reader-backed
archives have no file, so the guarded `getDirectAccessInformation` returns invalid →
no Xapian DB.

## Phase A — small-index local copy (this branch's target)

Goal: when an archive is reader-backed (remote) AND its fulltext index is below a
threshold, materialize ONLY the index blob (via the reader — not the whole archive)
to a temp file and open Xapian on it through the existing `getDbFromAccessInfo` path.

### A1. libzim patch (extend `stream-reader-api.patch`)
In `FileImpl::loadXapianDb()` add a reader-backed branch:
1. Locate the fulltext index entry and its byte extent. The index lives at the
   reserved `fulltext/xapian` path (sized item, reachable by id, filtered out of
   `getEntryByPath`). Get its cluster/blob → offset+size **within the archive**
   (the same info `getDirectAccessInformation` computes for the local path; we need
   the offset relative to the reader, which addresses the whole archive at [0,getSize)).
2. If `size >= threshold` → leave as today (return nullptr for now; Phase B handles big).
3. Else: read `[offset, offset+size)` through `m_zimReader` (the `StreamFileReader`
   over the `IRandomAccessReader`) into a temp file (libzim already links a temp-file
   helper; otherwise `std::tmpfile`/`mkstemp`). Then `Xapian::Database(tempfd)` and
   cache the open DB on the `FileImpl` (one-time per archive; delete the temp file on
   close, or `unlink` after open on POSIX).
   - Title index (`title/xapian`) gets the same treatment for `zim_suggest` quality
     (optional; suggest already has a non-Xapian fallback).

### A2. threshold plumbing
- Add a field to libzim `OpenConfig` (e.g. `size_t maxLocalSearchIndexBytes = 0`), read
  in `loadXapianDb`. `0` = never local-copy (range-read only, Phase B); large = always.
- Extension side: a DuckDB setting `zim_remote_search_max_local_index` (bytes; default
  ~32 MB). `zim_remote.cpp` passes it into the `Archive(reader, OpenConfig)` ctor when
  opening a remote archive. (Native/local archives keep the mmap fast path untouched.)

### A3. extension wiring
- `zim_access.cpp`/`zim_archive_pool.cpp`: thread the setting through to where remote
  archives are opened with the reader. No new SQL surface — `zim_search` already calls
  the libzim `Searcher`; it just starts returning rows remotely for small indexes.

### A4. tests (the gating prerequisite)
- **Need a small ZIM WITH a fulltext index.** `test/oracle/test.zim` (3 articles) has
  none. Extend `test/oracle/make_fixture.py` (python-libzim) to emit a fixture built
  **with** a fulltext index (or use `zimwriterfs`), e.g. `test_fts.zim`.
- Add `test/sql/zim_remote_search.test`: serve the fixture over `file://` or a local
  http server, `zim_search('http://…/test_fts.zim', 'term')` → expected rows. Also a
  case with the threshold set to 0 (expect no rows / Phase-B-only) and above the index
  size (expect rows).

## Phase B — random-access glass reader (follow-on, big indexes)

Per `remote-search-design.md`: patch Xapian's glass backend to read 8 KB blocks through
a `RandomAccessReader` (the same `getSize`/`readAt` shape), backed by the httpfs
`FileHandle` at `index_offset + block*8192`; keep the `Xapian::Database` warm; rely on
`cache_httpfs` (verify ≤64 KB granularity). Removes the local-fd requirement entirely.
Larger: a Xapian vcpkg overlay patch + libzim wiring to pass the reader to Xapian.

## Build / verify loop
- libzim source for patch authoring: `~/Projects/libzim` (branch `stream-reader-api`).
- Iterate the patch as `vcpkg_ports/libzim/*.patch`; `vcpkg_binary_sources: clear` (CI
  already uses this) forces a rebuild. Native first (`make release` + the new test),
  then the wasm angle composes with the LINKED_LIBS fix.

## Phase A status & findings (implemented + verified)

Verified end-to-end against `restarters_en_all_maxi` (17 MB, 589 KB fulltext index)
served over HTTP: remote `zim_search` returns the same ranked hits as local search.

- **Setting**: `zim_remote_search_max_local_index` (UBIGINT bytes, default **8 MB**,
  `0` disables remote search). Lowered from 32 MB to keep first-search latency snappy
  on broadband (32 MB ≈ 2.7 s download @100 Mbps). Read in `zim_search` and threaded
  through `ArchivePool::Get → ZimArchive::Open → OpenConfig::maxLocalSearchIndexBytes`.
- **The real search guard was `Archive::hasFulltextIndex()`** (returned
  direct-access validity = false for reader-backed). Fixed via
  `FileImpl::canCopySearchIndexLocally` (in the libzim patch).
- **Over-fetch (response time)**: httpfs reads in **intrinsic 1 MiB range blocks**, so
  libzim's scattered reads amplify ~15× (read_zim: 820 KB logical → 12.5 MB physical;
  search: ~5.3 MB of 17 MB). NOT tunable via `streaming_buffer_size` or
  `enable_external_file_cache` (both tested, no effect). This is an httpfs-layer
  artifact, independent of the local-copy logic — a separate optimization (httpfs
  block-size knob / read coalescing, possibly upstream).
- **Lazy index load deferred**: `OpenConfig::preloadXapianDb(false)` would skip the
  index fetch for non-search remote queries, but libzim's lazy `getXapianDb()` returns
  null for reader-backed archives (search then throws "Cannot create Search without FT
  Xapian index"). Left eager for now (bounded: index ≤ cap); revisit once that libzim
  path is understood.
