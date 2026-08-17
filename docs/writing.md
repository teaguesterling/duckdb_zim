# Writing archives

`COPY (query) TO 'out.zim' (FORMAT zim [, options])` turns the result of any query into a
new ZIM archive.

```sql
COPY (SELECT path, title, mimetype, content FROM articles)
TO 'out.zim' (FORMAT zim, TITLE 'My Archive', LANGUAGE 'eng', INDEX true);
```

The design goal is that `read_zim`'s output schema is `COPY`'s input schema, so copying an
existing archive is close to an identity:

```sql
COPY (SELECT * FROM read_zim('in.zim', include_content := true))
TO 'copy.zim' (FORMAT zim);
```

!!! note "Read content as BLOB, never `content_as_varchar := true`, when copying"
    `content_as_varchar` returns `NULL` for any entry whose bytes aren't valid UTF-8 —
    every image, font, and other binary file in a real archive. `COPY` **refuses** a NULL
    `content` on a non-redirect row, naming the entry, so piping that into a copy fails
    rather than silently writing those entries empty. `content` is `BLOB` by default (as in
    the example above); just don't add `content_as_varchar := true` to a `read_zim()` call
    that feeds a `COPY`.

!!! warning "A whole-archive copy does **not** carry metadata, the illustration, or the main entry"
    The example above copies every *entry*, and that is all it copies. `Title`,
    `Language`, `Description`, the illustration and the main page live in archive-level
    state that has no column in the input relation — they come only from options. So the
    copy's `zim_metadata_keys()` returns `[Counter]` alone and `zim_main_entry()` returns
    `NULL`, even though every entry and every byte of content matched.

    Nor is the fulltext index carried over: rebuild it with `INDEX true` if you want one.
    Measured on the test fixture, `all_entry_count` drops 16 → 9 across a round trip (five
    metadata entries plus two Xapian index entries), while `entry_count` and
    `zim_counter()` are unchanged. Pass the metadata you want to keep as options:

    ```sql
    COPY (SELECT * FROM read_zim('in.zim', include_content := true))
    TO 'copy.zim' (FORMAT zim, TITLE 'My Archive', LANGUAGE 'eng', INDEX true,
                   MAIN_PATH 'A/Home');
    ```

## Input columns

Columns are resolved **by name**, case-insensitively.

| Column | Type | Required | Meaning |
|---|---|---|---|
| `path` | VARCHAR | **yes** | entry path; must be unique within the archive |
| `content` | VARCHAR \| BLOB | **yes** | entry bytes; `NULL` only for a redirect row |
| `title` | VARCHAR | no | defaults to `path` |
| `mimetype` | VARCHAR | no | defaults from the `content` column's SQL type — `VARCHAR` → `text/plain`, `BLOB` → `application/octet-stream` |
| `is_redirect` | BOOLEAN | no | `true` writes a redirect entry instead of an item |
| `redirect_path` | VARCHAR | when `is_redirect` | the redirect's target path; must match an entry the query also produces |
| `front_article` | BOOLEAN | no | libzim's `FRONT_ARTICLE` hint |
| `compress` | BOOLEAN | no | libzim's `COMPRESS` hint |

`size` and `file_path` — two columns `read_zim` emits — are accepted and **ignored**:
`size` is derived by libzim, and `file_path` is provenance, not content. That's what lets
`read_zim`'s own output pipe straight back into `COPY` unmodified. Any *other* unrecognized
column is a bind-time error (a typo'd `titel` fails rather than silently producing an
untitled entry).

`content_path`, `entry_kind`, and `target` are columns planned for a future version (content
locators, aliases, and explicit redirect/alias typing). Naming one today is rejected with
"not supported yet" rather than "unknown column", so the deferral reads as a deferral rather
than a typo.

Column **types** are checked at bind time, before anything is written: a `path` that isn't
`VARCHAR`, or an `is_redirect` that isn't `BOOLEAN`, fails with an error naming the column
and both types. `content` accepts either `VARCHAR` or `BLOB` — that's the distinction the
`mimetype` default is derived from.

`content` may only be `NULL` on a redirect row. Anywhere else it's an error naming the
entry, because writing it as an empty entry would lose the data with no signal at all.

## Options

| Option | Type | Default | Meaning |
|---|---|---|---|
| `TITLE` | VARCHAR | — | `Title` metadata |
| `DESCRIPTION` | VARCHAR | — | `Description` metadata |
| `LANGUAGE` | VARCHAR | — | `Language` metadata (ISO 639-3, e.g. `eng`) |
| `CREATOR` | VARCHAR | — | `Creator` metadata |
| `PUBLISHER` | VARCHAR | — | `Publisher` metadata |
| `NAME` | VARCHAR | — | `Name` metadata |
| `DATE` | VARCHAR | — | `Date` metadata (`YYYY-MM-DD`) |
| `TAGS` | VARCHAR | — | `Tags` metadata |
| `METADATA` | MAP(VARCHAR, VARCHAR) | `{}` | arbitrary additional metadata keys |
| `ILLUSTRATION` | BLOB | — | cover image / favicon (48×48 PNG) |
| `MAIN_PATH` | VARCHAR | — | landing page; must match an entry the query produced |
| `INDEX` | BOOLEAN | `false` | build a Xapian fulltext index |
| `INDEX_LANGUAGE` | VARCHAR | `LANGUAGE` | stemming language for the index |
| `COMPRESSION` | VARCHAR | `zstd` | `'zstd'` or `'none'` |
| `CLUSTER_SIZE` | BIGINT | libzim default | target uncompressed cluster size in bytes; must be ≥ 1 — omit it for libzim's own default |
| `WORKERS` | BIGINT | `4` | libzim's internal worker count; 1–256 |
| `ON_CONFLICT` | VARCHAR | `'error'` | duplicate-`path` policy: `'error'` or `'first'` |

A named metadata option (`TITLE`, `LANGUAGE`, …) and the equivalent `METADATA` key are the
same setting; supplying both for one key is a bind-time error rather than last-wins. A
`NULL` value is rejected for every option, including for an individual `METADATA` map value
— never coerced to the string `"NULL"` or to a default.

!!! note "`COMPRESSION` has two values, not three"
    libzim 9.7.0 removed LZMA from the ZIM format entirely — `zim/zim.h` declares only
    `enum class Compression { None = 1, Zstd = 5 }`, with the intermediate values marked as
    no-longer-supported. `COMPRESSION 'lzma'` is rejected by name, with an error explaining
    why, rather than silently falling back to `zstd`.

!!! note "`INDEX` needs a language"
    `INDEX` defaults to `false`. Setting it to `true` requires a language to stem against:
    it's taken from `LANGUAGE` if that option was given, or set explicitly with
    `INDEX_LANGUAGE`. If neither resolves, `COPY` fails at bind time rather than writing an
    unsearchable archive that was asked to be searchable. On a build with no Xapian support
    (WebAssembly), requesting `INDEX true` is also a bind-time error — never a silent no-op.

!!! note "`ON_CONFLICT 'last'` isn't offered"
    `'error'` (the default) fails on the first duplicate `path`, naming it. `'first'` keeps
    the first occurrence and skips the rest. `'last'` would require buffering every row
    before writing any — a later duplicate can only win if nothing has been written yet —
    so it's rejected at bind time rather than silently doing that buffering. Deduplicate in
    SQL first if you need last-wins.

`MAIN_PATH` and a redirect's `redirect_path` are both validated against the set of paths
the query actually produced, at the end of the write (`copy_to_finalize`). libzim itself
accepts either silently pointing nowhere — `setMainPath()` to an unknown path completes
with `has_main_entry = false` and no error, and `addRedirection()` to an unknown target is
silently dropped at `finishZimCreation()`. Both would produce a working-looking archive
that quietly lost information, so `COPY` rejects them instead, naming the offending path.

## Writing never overwrites

If the output path already exists, `COPY` fails — a deliberate deviation from `parquet` /
`csv`, which clobber by default. A ZIM is often the only copy of a corpus that took hours to
build, and the format has no way to record "this is incomplete": a truncated archive that
*did* get finalized opens, checksums and passes `zim_check()` exactly like a complete one.
Clobbering would therefore destroy the known-good copy before anything could establish that
the replacement was whole. Remove the file yourself first if you mean to replace it.

`OVERWRITE`, `OVERWRITE_OR_IGNORE` and `APPEND` don't change this. DuckDB accepts them at
the `COPY` level for every format and consumes them before the zim writer is bound, so they
have no effect here — the refusal says as much rather than leaving you to wonder.

This also means a source archive can never be a valid `COPY` target — a query reading
`a.zim` can't accidentally truncate it by writing back to `'a.zim'`, because `a.zim`
already exists.

On any error partway through a write — a cast failure mid-stream, a duplicate path, a NULL
`content`, an invalid `MAIN_PATH` or redirect target — no file is left at the output path.
libzim writes to a `.tmp` sibling and only renames it into place once the archive is
complete, and `COPY` never finalizes while unwinding, so a failed write cannot leave a
plausible-looking archive behind.

The converse is checked too: a `COPY` that reports success has definitely produced a file.
libzim's final rename does not report failure, so if the output path is a **directory**, or
is not writable, the write can run to completion and land nowhere. `COPY` verifies the
archive exists before it reports success and raises otherwise, naming the output path:

```
COPY TO (FORMAT zim): no archive was written to 'out.zim'. …
```

The most likely way to meet this is a rejected `PER_THREAD_OUTPUT` or `PARTITION_BY` write:
DuckDB creates a directory at the output path before the zim writer gets to refuse the
option, so the same path is a directory when you retry without it. Remove the directory
before retrying.

## Aliases are not preserved

An alias in a source archive shares its target's stored bytes, but `read_zim` can't tell an
alias apart from a normal item — same `is_redirect = false`, same content, same size. A
round trip through `COPY` therefore writes it back as a full duplicate item rather than an
alias: the archive stays semantically equivalent to a reader, but grows a second copy of
the data and its `entry_count` changes. A round-trip comparison that only checks content
will not catch this; compare `entry_count` and `zim_counter()` too. (`all_entry_count`
always differs across a round trip for the unrelated reason above — dropped metadata — so
it can't serve as an alias check.)

## Not supported yet

- `content_path` (loading content from a local file or another archive lazily)
- `entry_kind` / `target` (explicit alias and redirect-kind columns)
- `PARTITION_BY` (one archive per key)

Each is rejected with a "not supported yet" error rather than being silently ignored.

`PER_THREAD_OUTPUT` is rejected outright rather than deferred: it would write one archive
per thread, each with its own duplicate-path detection, its own `MAIN_PATH` validation and
its own copy of the metadata — an arbitrary split of one corpus across files that the format
has no way to express.
