# duckdb_zim — developer handoff (HISTORICAL)

> **Historical.** This was the phase-1 kickoff handoff. Phases 1–3 are now built,
> tested, and merged (content scan, metadata, the `zim://` filesystem, and Xapian
> full-text search). Kept for the design/reading-order context below; for current
> usage see `README.md` and for rationale see `DESIGN.md`.

You are picking up **duckdb_zim**, a DuckDB extension that reads `.zim` files
(Kiwix / openZIM archives) via **libzim**. The design is done, the libzim semantics are
verified, and the **phase-1 core is written**. Your job is to drop it into a DuckDB
extension template, wire the build, get it compiling, and make the tests pass — then
proceed to phase 2.

Can double as the repo's `CLAUDE.md` for a Claude Code session.

## Read first, in this order

1. `README.md` — what the extension does and its phase-1 surface.
2. `docs/libzim-semantics.md` — **authoritative**, empirically verified libzim-7
   behavior. Where it and `DESIGN.md` disagree, this wins.
3. `DESIGN.md` — design rationale and the phase 2–4 roadmap. Its §3/§4.1 namespace
   details are **superseded** (banner at top points here).

## What's already done (don't redo)

- The **verification gate** is complete: namespace/path semantics, metadata, Counter
  format, search, counts — all confirmed against a real archive with python-libzim and
  recorded in `docs/libzim-semantics.md`. The big finding: content paths are
  namespace-free; `M/`/`W/`/`X/` are not path-addressable; metadata is a separate door.
- **Phase-1 core source** (`src/`):
  - `zim_access.{hpp,cpp}` — the libzim wrapper; the ONLY place (with the pool) that
    includes `<zim/*>`. Encodes the verified semantics. Returns DuckDB-agnostic structs.
  - `zim_archive_pool.{hpp,cpp}` — process-wide warm-handle cache (keeps libzim's
    cluster cache hot across queries; shared by every surface).
  - `read_zim.cpp` — content table function: projection pushdown, lazy content,
    prefix listing (`path_prefix`/`title_prefix`), exact lookup (`path`/`title`).
  - `zim_metadata.cpp` — `read_zim_metadata` + `zim_metadata`/`_keys`/`zim_counter`/`zim_info`.
  - `zim_scalars.cpp` — `zim_get_content`/`_text`/`has_entry`/`redirect_target`/`mimetype`/`main_entry`.
  - `zim_extension.cpp` — registration glue + entry points.
- **Tests** (`test/sql/*.test`) — sqllogictest, expected values are the verified oracle
  outputs. `test/oracle/make_fixture.py` regenerates the fixture (`pip install libzim`).

## Your task, in dependency order

### 1. Scaffold + integrate
Generate the repo from `duckdb/extension-template` (extension name `zim`). Drop in `src/`,
`test/`, `docs/`, `README.md`, `DESIGN.md`. Regenerate the fixture in-tree:
`python3 test/oracle/make_fixture.py` (needs `pip install libzim`). The shipped
`test/oracle/test.zim` was built by that same script.

### 2. Wire the build (modify the template's files — do not replace wholesale)

**`vcpkg.json`** — add libzim:
```json
{ "name": "libzim", "features": ["xapian"] }
```
libzim may not be in the template's pinned vcpkg baseline; if `vcpkg install` can't find
it, add an overlay port or bump the baseline. (xapian gives libzim working full-text
search, used in phase 3; it's harmless now.)

**`CMakeLists.txt`** — add the new sources and link libzim. libzim ships `libzim.pc`, so
pkg-config is the reliable path:
```cmake
set(EXTENSION_SOURCES
    src/zim_extension.cpp
    src/zim_access.cpp
    src/zim_archive_pool.cpp
    src/read_zim.cpp
    src/zim_metadata.cpp
    src/zim_scalars.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIM REQUIRED IMPORTED_TARGET libzim)   # VERIFY target/module name
# link PkgConfig::LIBZIM into BOTH the static and loadable targets the template defines:
target_link_libraries(${EXTENSION_NAME} PkgConfig::LIBZIM)
target_link_libraries(${LOADABLE_EXTENSION_NAME} PkgConfig::LIBZIM)
```
(If the vcpkg port exposes a CMake config instead, use `find_package(libzim CONFIG REQUIRED)`
and link `libzim::libzim`. Confirm which against the installed port.)

`ZIM_WITH_XAPIAN` as a CMake option is deferred to phase 3 / the WASM spike; phase-1 code
compiles regardless of how libzim was built (the search headers always exist).

### 3. First compile — expect to fix these (the work is here)
The libzim access and DuckDB binding were written to current conventions but **not
compiled**. Resolve, against the pinned versions:
- libzim C++ spellings marked `// VERIFY:` in `zim_access.cpp` — `Uuid`->`std::string`,
  `findByPath`/`findByTitle` signatures, the `Searcher`/`Query`/`SearchResultSet`
  iterator (its `getPath`/`getTitle`/`getScore`/`getSnippet`; score/snippet are left
  commented out — enable once confirmed).
- DuckDB binding spellings — `ExtensionUtil::RegisterFunction`, `Value::MAP`/`STRUCT`,
  `child_list_t`, `string_t`, `TableFunctionInitInput::column_ids`,
  `projection_pushdown`. Cross-check against how the `markdown`/`webbed` extensions spell
  these for the DuckDB version you pin.
- Keep all libzim contact inside `zim_access.cpp`/`zim_archive_pool.cpp`. If a binding
  file needs a zim type, add a method to `ZimArchive` instead.

### 4. Run tests, fix expectations
`make test`. Two categories are most likely to need adjustment:
- `test/sql/zim_errors.test` — `statement error` matches a substring of the message;
  DuckDB may reformat the exception text. Adjust substrings to match actual output.
- `test/sql/read_zim.test` — `title_prefix := 'C'` -> 2 depends on `findByTitle` prefix
  scoping (not confirmable via python). Verify; fix the count if needed.

### 5. Then phase 2+
Only after green: `zim://` filesystem (content-path-first grammar per the findings doc),
then `zim_search` (phase 3), then `ATTACH TYPE zim` (phase 4). Don't build these on an
uncompiled base.

## Hard constraints (do not violate)

- **License: GPL-2.0-or-later** (libzim). Intentional; separate from the MIT family.
- **No HTML/XML/markdown parsing in this binary.** Content out is BLOB/VARCHAR. Parsing
  is `webbed`'s job via the (phase-2) `zim://` filesystem. This keeps the GPL surface
  confined to "read the container" and avoids duplicating webbed.
- **No dependency on webbed/markdown/duck_block_utils in the binary.** Integration is
  documented recipes + (optional) a SQL macro pack, never a link dependency.
- **No `namespace` column; no custom `ZIM` type.** (See findings doc for why.)
- **`content` is lazy** — fetching a blob during a listing scan is a bug.
- Mirror the `markdown` extension's conventions: `read_zim*` table fns, `zim_*` scalars,
  named `:=` params, `include_filepath`/`filename` alias.

## Working style (the human's stated preferences)

- Be frank; surface uncomfortable findings and disagreements immediately.
- Flag intentional deviations and anything you feel strongly is wrong/suboptimal.
- Prefer concrete artifacts (a failing test, a diff) over abstract plans; small iterative
  steps; expect targeted scope corrections. Give next actions, not just plans.
- If the environment blocks something (network, vcpkg), say so plainly rather than
  faking progress.

## Definition of done for this pass
Extension builds from the template against a pinned DuckDB + vcpkg libzim; all five
`test/sql/*.test` files pass against the regenerated fixture; `// VERIFY` spots resolved;
error-message substrings corrected. Report what changed (especially any place the verified
semantics or the binding API differed from what was written) before moving to phase 2.
