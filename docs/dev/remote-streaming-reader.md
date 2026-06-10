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

## The libzim patch (~120 LOC)

1. **`include/zim/istreamreader.h`** (NEW, public `LIBZIM_API`) — the only new public symbol:
   ```cpp
   namespace zim {
     class LIBZIM_API IStreamReader {
     public:
       virtual ~IStreamReader();
       virtual size_type getSize() const = 0;
       virtual void readAt(char* dest, offset_type offset, size_type size) const = 0; // threadsafe
     };
   }
   ```
2. **`include/zim/archive.h`** — `Archive(std::shared_ptr<IStreamReader> reader, OpenConfig = {});`
3. **`src/stream_reader.{h,cpp}`** (NEW, `LIBZIM_PRIVATE_API`) — `StreamReader : BaseFileReader`,
   copied from `FileReader`: `readImpl(dest,off,n)` → `m_stream->readAt(m_base+off, n)`;
   `sub_reader(off,n)` → offset-shifted `StreamReader`; `offset()`→m_base, `size()`→m_size,
   `getMemorySize()`→0.
4. **`src/fileimpl.{h,cpp}`** — `FileImpl(shared_ptr<Reader>, OpenConfig)`: extract the
   post-reader init the fd ctors run into a shared helper; new ctor sets `zimReader` then runs it.
5. **`src/archive.cpp`** — `Archive(shared_ptr<IStreamReader> r, OpenConfig oc) : m_impl(new
   FileImpl(std::make_shared<StreamReader>(r), oc)) {}`
6. **`meson.build`** — new sources + install the new public header.

Scope-out: single `IStreamReader` = single-part archive (remote split archives are rare).

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

- **Local-via-reader**: open a local `.zim` through the reader path and diff against mmap
  (same entries/content) — proves the `StreamReader` adapter in isolation.
- **HTTP**: `python -m http.server` over `test/oracle/test.zim` → `read_zim('http://localhost:PORT/test.zim')`;
  entries, content, search, parallel.
- **S3**: if creds available.
- **Range-read proof**: instrument `readAt` byte totals — a listing scan must read ≪ filesize
  (proves "partial", not whole-file).
- The NHS↔Wikipedia cross-reference, but with the remote archive.

## Open questions

- httpfs `FileHandle`: positioned `Read(buf,n,location)` concurrency-safe? → mutex vs per-thread handle.
- `BaseFileReader::get_buffer` non-mmap correctness across *all* access (clusters, dirents, xapian)?
- Route local through the reader too (uniformity) or keep mmap (perf)? → keep mmap.

## Checklist

- [ ] Open openZIM/libzim issue proposing `IStreamReader`.
- [ ] Fork libzim; implement patch; tiny C++ test (open from an in-memory `IStreamReader`).
- [ ] Generate `stream-reader-api.patch`; add `vcpkg_ports/libzim` overlay; wire `overlay-ports`.
- [ ] `DuckdbZimStreamReader` + `ZimArchive::Open` remote path + `ArchivePool` plumbing.
- [ ] Remote-URL detection + `enable_external_access` + httpfs-missing error.
- [ ] Tests (local-via-reader, http server, parallel, byte-count proof).
- [ ] Docs (installation: httpfs; reading: remote archives).
- [ ] Upstream PR.
