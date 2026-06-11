# Worksheet — remote ZIM via a streaming reader

**Goal:** `read_zim('s3://…/x.zim')` / `https://…` and `zim://` read archives on S3/HTTP,
fetching only the byte ranges a query touches (dirents + clusters) — *not* the whole file.
Reuse DuckDB httpfs's range-read + auth machinery.

**Decisions (locked):**
- ❌ No whole-file fetch-to-local cache (huge files; footgun). True lazy range reads only.
- ⏭️ Skip the "fail-clean-on-remote" interim guard — go straight to the real reader.
- Local files keep the **mmap** fast path; only remote routes through the bridge.

---

## Upstream status (checked 2026-06)

No existing libzim issue/PR for a pluggable/custom Reader or remote/streaming open. Nearest:
[#789](https://github.com/openzim/libzim/issues/789) (compile reader-only — modularity, not
I/O), [#1058](https://github.com/openzim/libzim/issues/1058) (mmap on macOS, closed),
[#879](https://github.com/openzim/libzim/issues/879) (split-archive `FdInput` opening). →
`IStreamReader` is net-new. Plan: open an issue proposing it, ship as a vcpkg overlay patch
in the meantime, offer the PR.

## Prior art (the template)

`duckdb_spatial`'s GDAL bridge — `DuckDBFileHandle : VSIVirtualHandle` over a
`duckdb::FileHandle`, registered under a `/vsiduckdb-<uuid>/` prefix
(`src/spatial/modules/gdal/gdal_module.cpp`). Copy its `Seek`/`Read-at-offset` structure.
sqlite_scanner is local-only (no VFS bridge); anndata uses HDF5's VFD but reimplements its
own libcurl (not a DuckDB-FS bridge). The decisive constraint: **libzim, unlike SQLite
(VFS) / HDF5 (VFD), has no public I/O hook** — so step 1 is *adding* one.

## libzim internals (verified, src 9.7.0)

- `Reader` (`src/reader.h`): abstract; core op `readImpl(char* dest, offset_t off, zsize_t size)`
  (positioned read) ≡ `FileHandle::Read(buf, n, location)`. Plus `get_buffer`, `sub_reader`,
  `size`, `offset`, `getMemorySize`.
- `BaseFileReader : Reader` (`src/file_reader.h`): shared `get_buffer`; the fd `FileReader`
  proves the non-mmap allocate-and-read path works.
- `FileReader : BaseFileReader`: fd→Reader adapter (`pread`). **Our adapter template.**
- `FileImpl` holds `shared_ptr<Reader> zimReader`; ctors fname/fd/FdInput/vector<FdInput>.
- `Archive` ctors → `new FileImpl(...)`.

## VERIFIED against libzim 9.7.0 source (2026-06-10)

Cloned `openzim/libzim@9.7.0` → `~/Projects/libzim` (branch `stream-reader-api`). Findings that
**revise** the original plan:

1. **`IStreamReader` is already taken.** `src/istreamreader.{h,cpp}` defines a `LIBZIM_PRIVATE_API`
   *sequential* primitive-stream reader (`readImpl(buf, nbytes)`, no offset). Our positioned-read
   public interface must use a different name → **`IRandomAccessReader`**.
2. **The positioned-read seam is the internal `Reader`** (`src/reader.h`, `LIBZIM_PRIVATE_API`):
   `readImpl(dest, offset_t, zsize_t)`. `BaseFileReader : Reader` (`src/file_reader.{h,cpp}`) adds
   offset/size + an abstract `get_mmap_buffer`. `FileReader : BaseFileReader` reads via
   `_fhandle->readAt(dest, size, offset)` — our adapter template.
3. **Non-mmap `get_buffer` already exists and works.** `BaseFileReader::get_buffer`
   (file_reader.cpp:188) catches `MMapException` (or compiles it out when `ENABLE_USE_MMAP` is off)
   and falls back to *allocate + positioned `read()`*. So header/dirents/clusters/content all flow
   through the Reader with no mmap. ⚠️ `MMapException` lives in an **anonymous namespace inside
   file_reader.cpp** → our adapter must live in that TU (or we hoist the exception) so its
   `get_mmap_buffer` can `throw MMapException` to trigger the fallback.
4. **FileImpl is coupled to `FileCompound` (`zimFile`) beyond reading.** All public ctors funnel
   into the private `FileImpl(shared_ptr<FileCompound>, OpenConfig)` (fileimpl.cpp:197). `zimFile`
   is used in **6 spots** our reader-backed ctor must guard (set `zimFile=nullptr`, store a name):
   `getFilename()` (.h → stored name), `getMTime()` (→ 0), `is_multiPart()` (→ false),
   `getFileParts()`/`getDirectAccessInformation()` (→ invalid `ItemDataDirectAccessInfo`),
   `verify()` (→ false/unsupported; do **not** rewrite — that would whole-file download remotely),
   and the ctor log/fail-check. `getFilesize()`/`getChecksum()` already use `zimReader` — fine.
5. **🔴 Xapian needs a real fd — remote full-text search is OUT of scope for v1.**
   `loadXapianDb()` (fileimpl.cpp:883) → `getDirectAccessInformation()` → `getDbFromAccessInfo()`
   (tools.cpp:320) which does `openFile(accessInfo.filename)` + `seek(offset)` +
   `Xapian::Database(fd)`. Xapian mmaps the **OS file directly**; it cannot read through our reader.
   With the guard, `getDirectAccessInformation` returns invalid → `loadXapianDb` returns `nullptr`
   → `zim_search` yields **no rows, gracefully** (no crash). `zim_suggest` still works remotely via
   its non-Xapian prefix fallback (dirent title lookup, through the reader).
   - **Future remote-search enhancement** (doesn't break "no whole-file download"): the Xapian index
     is one contiguous *uncompressed* blob (tens of MB), readable *through the reader*. Extract it to
     a temp file and point Xapian at that. Follow-up, not v1.

## The libzim patch

1. **`include/zim/irandomaccessreader.h`** (NEW, public `LIBZIM_API`) — the only new public symbol:
   ```cpp
   namespace zim {
     class LIBZIM_API IRandomAccessReader {
     public:
       virtual ~IRandomAccessReader();
       virtual size_type getSize() const = 0;
       virtual void readAt(char* dest, offset_type offset, size_type size) const = 0; // threadsafe
     };
   }
   ```
2. **`include/zim/archive.h`** — `Archive(std::shared_ptr<IRandomAccessReader> reader, OpenConfig = {});`
3. **`src/file_reader.{h,cpp}`** (extend; lives here for `MMapException` access) —
   `StreamFileReader : BaseFileReader`, mirroring `FileReader`: `readImpl(dest,off,n)` →
   `m_source->readAt(dest, (m_base+off), n)`; `get_mmap_buffer` → `throw MMapException` (force the
   allocate+read fallback); `sub_reader(off,n)` → offset-shifted `StreamFileReader`;
   `offset()`→m_base, `size()`→m_size.
4. **`src/fileimpl.{h,cpp}`** — `FileImpl(shared_ptr<Reader>, std::string name, OpenConfig)`: set
   `zimReader`, `zimFile=nullptr`, store name; guard the 6 `zimFile` spots above. (Extract shared
   post-reader init only if the existing private ctor can't be reused as-is.)
5. **`src/archive.cpp`** — `Archive(shared_ptr<IRandomAccessReader> r, OpenConfig oc)` → builds a
   `StreamFileReader` over `r` (size = `r->getSize()`), passes it + a display name to `FileImpl`.
6. **`meson.build`** — install the new public header (`include/zim/meson.build` headers list); the
   adapter rides existing `file_reader.cpp`.

Scope-out: single `IRandomAccessReader` = single-part archive (remote split archives are rare).

## vcpkg bundling (carry the patch before upstream)

- New overlay `vcpkg_ports/libzim/` in the repo: copy the registry `portfile.cmake` + `vcpkg.json`
  (they already carry `cross-builds.diff` / `dllexport.diff` / `subdirs.diff`); add
  `stream-reader-api.patch` to the `vcpkg_from_github(... PATCHES ...)` list.
- `vcpkg.json` `overlay-ports`: add `./vcpkg_ports` (alongside `./extension-ci-tools/...`).
- Patch = `git diff` of our libzim fork branch.
- On upstream merge + newer `vcpkg_commit`: delete the overlay. Done.

## Extension side

- **`DuckdbZimStreamReader : zim::IStreamReader`** (new `src/zim_remote.{cpp,hpp}`): holds
  `unique_ptr<FileHandle>` + `FileSystem&`. `getSize()`→`fs.GetFileSize(*fh)`;
  `readAt(dest,off,n)`→ mutex-guarded `fs.Read(*fh, dest, n, off)`. (Mutex: httpfs positioned
  reads aren't guaranteed concurrent-safe; remote is I/O-bound; libzim caches absorb most reads.)
- **`ZimArchive::Open`**: branch on remote. Local → `zim::Archive(path)` (mmap, unchanged).
  Remote → FS `OpenFile` → `zim::Archive(make_shared<DuckdbZimStreamReader>(...))`.
- **Plumbing problem**: `ArchivePool` is process-wide + context-free, but remote needs a
  `FileSystem&`. Cleanest: `ArchivePool::Get(path, optional<FileSystem*>)`; `read_zim` /
  filesystem bind pass `FileSystem::GetFileSystem(context)`; local opens ignore it. Remote
  detection: `FileSystem::IsRemoteFile(path)` or scheme ∈ {s3, http, https, gcs, …}.
- **Guards**: respect `enable_external_access`; if httpfs not loaded, surface a clear
  "INSTALL httpfs" error rather than a libzim open failure.

## Parallel-scan interaction (v0.3)

Concurrent morsels call `readAt` → the mutex serializes remote I/O (fine: network/HDD-bound),
while decompression/parsing still parallelizes per morsel. Later: per-thread `FileHandle` (or
confirm httpfs concurrent-safe positioned reads) to parallelize the fetches too.

## Bonus

The reader path is mmap-free → it may enable **remote reads on Wasm** (no mmap there). Worth
a look once it works natively.

## Testing

- **C++ unit test (in libzim fork)**: open `test/oracle/test.zim` via an in-memory/fd-backed
  `IRandomAccessReader` and diff **full enumeration + content** against the same archive opened
  normally. This isolates the adapter + the 6 `zimFile` guards. (Search is *expected* to return
  nothing on the reader path — see finding 5; assert that, don't treat it as a bug.)
- **Local-via-reader (extension)**: same diff through the extension's remote bridge against a
  `file://`-ish local path forced down the reader branch.
- **HTTP**: `python -m http.server` over `test/oracle/test.zim` →
  `read_zim('http://localhost:PORT/test.zim')`; entries, content, **suggest (prefix)**, parallel.
  `zim_search` → expect 0 rows remotely (documented limitation), not an error.
- **S3**: if creds available.
- **Range-read proof**: instrument `readAt` byte totals — a listing scan must read ≪ filesize
  (proves "partial", not whole-file).
- The NHS↔Wikipedia cross-reference, but with the remote archive (uses content/lookup, not search).

## Open questions

- httpfs `FileHandle`: positioned `Read(buf,n,location)` concurrency-safe? → mutex vs per-thread handle.
- ~~`get_buffer` non-mmap correctness~~ → **resolved**: the fallback path already exists and serves
  header/dirents/clusters/content. Xapian is the only bypass (finding 5), handled by scope.
- Route local through the reader too (uniformity) or keep mmap (perf)? → keep mmap.

## Checklist

- [x] Clone libzim @ 9.7.0 (`~/Projects/libzim`, branch `stream-reader-api`); map the patch surface.
- [x] Implement patch (`IRandomAccessReader` header, `StreamFileReader` adapter, `FileImpl` ctor +
      6 guards, `Archive` ctor, meson header install). Builds clean; libzim fork `~/Projects/libzim`
      branch `stream-reader-api`, commit `10d2fcc`.
- [x] C++ proof harness (`test_reader_harness.cpp` in the fork): open `test.zim`/`test_zimit.zim`
      via an fd-backed `IRandomAccessReader`, diff full enumeration + content vs a normal open → MATCH.
      (Keep as the basis for a proper gtest in the upstream PR.)
- [x] Generate `stream-reader-api.patch`; add `vcpkg_ports/libzim` overlay; wire `overlay-ports`.
      Verified: all four patches apply cleanly in vcpkg order against pristine 9.7.0; the extension's
      vcpkg build installs the patched header and links (xapian on); 13 suites green.
- [x] `DuckdbZimRemoteReader : IRandomAccessReader` + `ZimArchive::Open` remote path + `ArchivePool`
      plumbing (per-call `FileSystem*` + a default registered at extension load → scalars work too).
- [x] Remote-URL detection (`FileSystem::IsRemoteFile`) + httpfs-missing remediation error.
- [x] Manual end-to-end proof: 982 MB remote Wikipedia on dumps.wikimedia.org — open + 1 article =
      5.3 MB (0.5%); local fixtures byte-identical remote vs local (serial+parallel, content,
      metadata, scalars, suggest); `zim_search` → 0 rows. **Automated remote sqllogictest still TODO**
      (needs a live range server in the harness).
- [x] Docs (reading.md "Remote archives", reference.md note, README roadmap row).
- [ ] **Known rough edges (deferred):** parallel scan over remote pre-reads all dirents (bad for
      `LIMIT`); scalars re-open per call (no warm pin → repeated remote opens). Fix with the v0.4
      filter/`LIMIT` pushdown work.
- [ ] Open openZIM/libzim issue/PR proposing the interface — *with the working patch attached*
      (confirm with Teague first; it's an outward action).
