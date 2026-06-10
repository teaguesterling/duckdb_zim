# Installation

## From community-extensions

```sql
INSTALL zim FROM community;
LOAD zim;
```

The published community build tracks tagged releases. The `zim://` filesystem,
`zim_search` / `zim_suggest`, and the other phase 2–3 features land with **v0.2.0**; build
from source (below) for anything ahead of the latest published tag.

## Build from source

DuckDB and the extension-ci-tools are pulled in as submodules, and `libzim` comes from
[vcpkg](https://vcpkg.io):

```sh
git clone --recurse-submodules https://github.com/teaguesterling/duckdb_zim.git
cd duckdb_zim
make            # needs a vcpkg toolchain; see DESIGN.md for the toolchain details
```

Then load the built extension into an unsigned DuckDB:

```sql
LOAD 'build/release/extension/zim/zim.duckdb_extension';
```

## Platforms

| Platform | Full-text search | Notes |
|---|---|---|
| Linux x64 / arm64 | ✅ | xapian built in |
| macOS x64 / arm64 | ✅ | xapian built in |
| WebAssembly (mvp / eh / threads) | — | builds green, **search-less** (xapian is gated out of emscripten); `zim_search` returns no rows, `zim_suggest` falls back to a title-prefix listing |
| Windows | ✗ | not yet supported |
