# Function reference

## Table functions

### `read_zim(files [, params])`

One row per content entry. `files` is a path, a glob, or a `LIST(VARCHAR)`.

Returns: `path VARCHAR, title VARCHAR, mimetype VARCHAR, is_redirect BOOLEAN, redirect_path VARCHAR, size UBIGINT, content (BLOB|VARCHAR) [, file_path VARCHAR]`.

| Parameter | Type | Meaning |
|---|---|---|
| `include_content` | BOOLEAN \| VARCHAR \| LIST | `true`/`false`, or a mimetype / list of patterns (with `image/*`, `*/*`) to load content only for matching entries |
| `content_as_varchar` | BOOLEAN | make `content` VARCHAR (non-UTF-8 → `NULL`) instead of BLOB |
| `mimetype` | VARCHAR \| LIST | Accept-style row filter; exact, `image/*`, or `*/*` patterns |
| `path` / `title` | VARCHAR | exact lookup (emits 0/1 row per archive) |
| `path_prefix` / `title_prefix` | VARCHAR | prefix listing (backed by `findByPath` / `findByTitle`) |
| `listing` | VARCHAR | `'path'` (all entries) or `'title'` (front articles only) |
| `include_filepath` (alias `filename`) | BOOLEAN | add the `file_path` column |
| `parallel` | BOOLEAN | `true` (default): the path-order scan runs across DuckDB's `threads`, partitioned by libzim cluster order (row order not guaranteed). `false`: single-threaded, path-ordered. Lookups / prefixes / title listing are serial regardless. |

Use SQL `LIMIT` / `OFFSET` for bounding/pagination.

**Filter pushdown:** every `WHERE` predicate DuckDB pushes into the scan is evaluated by
the scan, on every column. Two of them additionally reduce how much of the archive is
read: `WHERE path = '…'` / `WHERE title = '…'` become a libzim exact lookup (equivalent to
`path :=` / `title :=`) instead of a full scan — a big win on remote archives (a few KB
instead of reading every dirent) — and `WHERE mimetype = …` / `IN (…)` skip non-matching
entries during the scan. Other predicates (`LIKE`, ranges, `IN` on path/title) still read
the whole listing; use `path_prefix :=` for remote prefix listings.

`files` may be a **remote** URL (`s3://`, `http(s)://`, `gcs://`, …): the archive is read
by byte-range requests via `httpfs` (only the touched bytes are fetched). Requires
`LOAD httpfs` and `enable_external_access`. See
[Remote archives](reading.md#remote-archives-s3-http). All functions below accept remote
URLs too; only `zim_search` (Xapian) is local-only.

### `read_zim_metadata(file [, include_filepath])`

Returns `key VARCHAR, value VARCHAR, raw BLOB [, file_path VARCHAR]`. `value` is text only
(binary metadata like an illustration is `NULL` in `value`, present in `raw`).

### `zim_search(files, query [, max_results := 25, result_offset := 0])`

Xapian full-text search. Returns `path, title, score DOUBLE, snippet VARCHAR, file VARCHAR`,
ranked. Federated over a glob / `LIST` (per-archive `max_results`). No rows on a no-index
archive or a search-less build.

### `zim_suggest(files, query [, max_results := 25, result_offset := 0])`

Title autocomplete. Returns `path, title, snippet VARCHAR, file VARCHAR`. Works on every
build (prefix fallback without Xapian). Federated like `zim_search`.

## Scalar functions

| Function | Returns | Notes |
|---|---|---|
| `zim_get_content(file, path)` | BLOB | follows redirects; `NULL` if absent |
| `zim_get_text(file, path)` | VARCHAR | text mimetypes only; `NULL` for binary (never mangled) |
| `zim_has_entry(file, path)` | BOOLEAN | |
| `zim_redirect_target(file, path)` | VARCHAR | `NULL` if not a redirect |
| `zim_mimetype(file, path)` | VARCHAR | |
| `zim_main_entry(file)` | VARCHAR | redirect-resolved landing path |
| `zim_metadata(file, key)` | VARCHAR | single metadata value; `NULL` if absent or binary |
| `zim_metadata_keys(file)` | LIST(VARCHAR) | |
| `zim_counter(file)` | MAP(VARCHAR, BIGINT) | self-describing mimetype histogram |
| `zim_info(file)` | STRUCT | counts, flags, uuid, filesize, … |
| `zim_illustration(file [, size])` | BLOB | cover image / favicon (default 48px); `NULL` if none |
| `zim_random(file)` | VARCHAR | a random entry's path |
| `zim_check(file)` | BOOLEAN | archive is openable and internally consistent; `false` (not an error) if it cannot be opened |

All VARCHAR outputs are **binary-safe**: non-UTF-8 bytes come back as `NULL` rather than
being mangled.

### `zim_check` — what it does and does not promise

`zim_check` is the one function whose purpose is to answer *"is this archive usable?"*,
so it answers rather than aborting the query:

| input | result |
|---|---|
| a good archive | `true` |
| an archive that opens but fails its integrity check | `false` |
| a file that is not a ZIM, is truncated, or is missing | `false` |
| `NULL` | `NULL` |

This makes it usable as a predicate over a shelf, which is the point — one bad file in a
directory no longer aborts the scan:

```sql
SELECT file, zim_check(file) AS readable
FROM glob('/data/zim/*.zim');
```

`TRY()` is **not** a substitute for this behavior. `TRY()` intercepts conversion and range
errors, not the exception raised from the archive-open path, so `TRY(zim_check(f))` on an
unopenable file fails exactly as the bare call would.

> **It verifies internal *consistency*, not *completeness*.** An archive whose writer died
> part-way through is self-consistent: it opens, its checksum validates, and `zim_check`
> returns `true` — while silently containing only the entries written before the failure.
> Nothing in the ZIM format records how many entries *should* have been there. If you need
> completeness, compare `zim_info(file).entry_count` against what you expect; `zim_check`
> cannot tell you.

Only the *open* is guarded. If `CheckIntegrity()` itself throws, that error still
propagates — that is a real fault, not an unreadable file.

## The `zim://` filesystem

Not a function — a registered read-only filesystem. See
[The zim:// filesystem](filesystem.md).
