# Tests

sqllogictest files under `test/sql/`, run by the DuckDB extension test harness
(`make test`).

## Fixture

Two generated archives:

- `test/oracle/test.zim` — mwoffliner-style (`A/Foo` paths), 5 items + 1 redirect +
  metadata + **fulltext index**. The primary fixture.
- `test/oracle/test_zimit.zim` — zimit / browsertrix-style (full-URL paths like
  `example.com/products/widget/`, trailing slashes) and **no fulltext index**, for the
  edge cases the primary fixture doesn't cover.

Expected values in the `.test` files are the verified outputs of these fixtures
(see `../docs/libzim-semantics.md`). Regenerate in-tree before running tests:

```sh
pip install libzim          # same C++ core the extension links
python3 test/oracle/make_fixture.py
python3 test/oracle/make_zimit_fixture.py
```

## Files

- `read_zim.test`            — content scan, ordering, prefix listing, exact lookup, redirects
- `zim_content.test`         — content laziness, projection, BLOB vs VARCHAR
- `zim_include_content.test` — `include_content := mimetype | [mimetypes]` gated loading
- `zim_multifile.test`       — globs, `LIST(VARCHAR)`, replacement scan across archives
- `zim_metadata.test`        — read_zim_metadata, zim_metadata/_keys/zim_counter/zim_info
- `zim_scalars.test`         — single-entry lookup scalars
- `zim_parallel.test`        — parallel `read_zim` scan matches the serial scan
- `zim_pushdown.test`        — `WHERE path/title/mimetype` filter pushdown == un-pushed full scan
- `zim_filesystem.test`      — the `zim://` virtual filesystem (read_text/read_blob, glob)
- `zim_search.test`          — `zim_search` Xapian full-text search (native builds)
- `zim_federated.test`       — multi-archive `zim_search` / `zim_suggest` (glob/LIST, `file` column)
- `zim_utilities.test`       — `zim_suggest` / `zim_illustration` / `zim_random` / `zim_check`
- `zim_zimit.test`           — zimit-style full-URL paths, trailing slashes, no-index search
- `zim_errors.test`          — binder validation + bad-archive handling
- `copy_zim.test`            — `COPY ... TO ... (FORMAT zim)`: write, round trip, metadata, index
- `copy_zim_errors.test`     — write refusals: existing output, duplicates, bad options

## stdout-cleanliness shell tests

sqllogictest compares query results via the API and never inspects process stdout, so two
regressions get their own shell scripts instead:

- `test/no_stdout_pollution.sh` — searching a ZIM whose language Xapian can't stem (e.g.
  Chinese) must not print libzim's `No stemming for language 'X'` to stdout (issue #21).
- `test/no_writer_stdout_pollution.sh` — writing a ZIM (`COPY ... TO ... (FORMAT zim)`)
  must not print any of libzim's writer `INFO()` lines (`Set entry indices`, `Index
  titles`, `Detect dangling redirects`, …) to stdout; `configVerbose(false)` doesn't gate
  that macro. Both regressions are covered by patches in `vcpkg_ports/libzim/`.

Run locally after a build: `bash test/no_stdout_pollution.sh` /
`bash test/no_writer_stdout_pollution.sh` (same `DUCKDB_BIN` / `ZIM_EXTENSION` overrides as
below).

## FileSystem-level C++ harnesses

Two behaviours are only reachable through the C++ `FileSystem` / `DatabaseInstance` API,
so they are compiled against the built `libduckdb` and run directly rather than through
sqllogictest:

- `test/vfs_ranged_reads.sh` (+ `.cpp`) — the `zim://` handle's ranged/positional reads
  (reads at an offset, at EOF, spanning the end, zero-length). `read_blob` / `read_text`
  only ever take the sequential overload, so SQL cannot reach these (issue #27).
- `test/pool_revalidate.sh` (+ `.cpp`) — `ArchivePool` revalidation (issue #38): a `.zim`
  replaced on disk must stop being served from the pool's cached handle, while a `zim://`
  handle already open over the old archive keeps serving it. sqllogictest has no
  file-manipulation directive, `COPY … (FORMAT zim)` refuses to overwrite an existing
  archive, and restarting the DB would clear the pool and hide the bug — so the swap has
  to happen between queries of one live `DatabaseInstance`.

Run locally after `make release`: `bash test/vfs_ranged_reads.sh` /
`bash test/pool_revalidate.sh`. Both link the locally built `libduckdb` under
`build/release/src/`, so unlike the scripts above they are not part of the
artifact-based CI jobs (which only download the loadable extension).

## Remote (http) integration test

`zim_search` over an http(s) URL can't be expressed in sqllogictest (no way to host a
server), so it lives as a shell script:

- `test/remote/remote_search.sh` — serves the committed `test/oracle/test.zim` over a
  local HTTP server and asserts a remote `zim_search` returns the same hits as the local
  `zim_search.test`. Exercises the reader-copy path (libzim copies the small Xapian index
  out through the reader; see `docs/dev/remote-search-impl-plan.md`).

Run locally after a build: `bash test/remote/remote_search.sh`
(override `DUCKDB_BIN` / `ZIM_EXTENSION` to point at a prebuilt shell + loadable extension).
In CI it runs as the `remote-search-test` job against the built artifact.
