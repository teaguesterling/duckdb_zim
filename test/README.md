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
