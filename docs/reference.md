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

Use SQL `LIMIT` / `OFFSET` for bounding/pagination.

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
| `zim_check(file)` | BOOLEAN | libzim archive integrity check |

All VARCHAR outputs are **binary-safe**: non-UTF-8 bytes come back as `NULL` rather than
being mangled.

## The `zim://` filesystem

Not a function — a registered read-only filesystem. See
[The zim:// filesystem](filesystem.md).
