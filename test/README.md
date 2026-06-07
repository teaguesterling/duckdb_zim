# Tests

sqllogictest files under `test/sql/`, run by the DuckDB extension test harness
(`make test`).

## Fixture

`test/oracle/test.zim` is a tiny generated archive (5 items + 1 redirect + metadata +
fulltext index). Expected values in the `.test` files are the verified outputs of this
fixture (see `../docs/libzim-semantics.md`).

Regenerate it in-tree before running tests:

```sh
pip install libzim          # same C++ core the extension links
python3 test/oracle/make_fixture.py
```

## Files

- `read_zim.test`     — content scan, ordering, prefix listing, exact lookup, redirects
- `zim_content.test`  — content laziness, projection, BLOB vs VARCHAR
- `zim_metadata.test` — read_zim_metadata, zim_metadata/_keys/zim_counter/zim_info
- `zim_scalars.test`  — single-entry lookup scalars
- `zim_errors.test`   — binder validation + bad-archive handling

Two assertion groups may need adjustment on first compile (flagged inline): the
`statement error` substrings in `zim_errors.test`, and the `title_prefix` count in
`read_zim.test` (depends on libzim `findByTitle` prefix scoping).
