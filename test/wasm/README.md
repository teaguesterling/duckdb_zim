# WASM build tests

Guards against the LINKED_LIBS class of WASM bug: the DuckDB-WASM build installs
but fails to load because libzim's symbols are left unresolved in the side module.

## Root cause

The loadable `zim.duckdb_extension.wasm` is produced by a separate
`emcc -sSIDE_MODULE=2 ... ${LINKED_LIBS}` step (duckdb's
`extension/extension_build_tools.cmake`) that links **only** the archives named in
this extension's `LINKED_LIBS`. The `target_link_libraries(... PkgConfig::LIBZIM)`
in `CMakeLists.txt` is honored for native builds but **ignored** for the wasm link,
so libzim's symbols (and its transitive zstd/lzma) become unresolved imports.

## Fix

`CMakeLists.txt` resolves libzim's static link set from pkg-config (release
archives) and feeds them to the wasm link via this extension's `LINKED_LIBS`,
wrapped in `-Wl,--start-group … --end-group` for ICU's circular refs. Notes:

- libzim's **read** path needs **zstd + lzma** (cluster decompression) but **not
  ICU** — ICU is only pulled by search/suggestion/title-normalization, which the
  core read surface doesn't exercise. So the linker dead-strips ICU and the
  `.wasm` stays ~0.5 MB (vs the 35 MB `libicudata.a` on disk).
- `PkgConfig::LIBZIM` is interface-only, so a `$<TARGET_FILE:…>` genexpr in
  `extension_config.cmake` would resolve empty — hence resolving the archives in
  `CMakeLists.txt` (before `build_loadable_extension`, which reads `LINKED_LIBS`).

## Test — static symbol check (runtime-free)

`check_wasm_imports.mjs` parses the built `.wasm` and fails if any dependency
symbol (libzim/zstd/lzma/icu/xapian) is **imported but not defined/exported** by
the module. It needs no duckdb-wasm runtime or duckdb-version match, so a failure
is unambiguously the LINKED_LIBS bug. Run after a wasm build:

```bash
make wasm_mvp                 # or wasm_eh / wasm_threads
test/wasm/run_wasm_checks.sh  # scans build/wasm_* for zim.duckdb_extension.wasm
```

Empirically: before the fix, 42 unresolved `zim::` symbols; after, 0.

## Not covered here (future work)

- **Runtime ZIM reading in WASM.** This check proves the module *loads* (symbols
  resolve). Actually *reading* an archive in the browser is separate: libzim's
  local path uses **mmap** (unavailable in wasm); the route is the remote
  `IRandomAccessReader` / httpfs seam (see `docs/dev/remote-streaming-reader.md`).
- **Live load test** in a duckdb-wasm engine (à la webbed/yaml) — deferred until
  the runtime path above lands and a version-matched engine is available.
- **Reuse DuckDB's ICU** instead of (dead-stripped) bundled ICU — see DESIGN §0.3.
