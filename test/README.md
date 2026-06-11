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
