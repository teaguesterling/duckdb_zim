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
    every image, font, and other binary file in a real archive. Pipe that into `COPY` and
    those entries are written **empty**, silently: no error, no warning, and a
    content-equality check even passes, because `NULL` was written on one side and read
    back as `NULL` on the other. `content` is `BLOB` by default (as in the example above);
    do not add `content_as_varchar := true` to a `read_zim()` call that feeds a `COPY`.
    This is the single easiest way to lose data with this feature.

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

`content_path`, `entry_kind`, `target`, `source_archive`, and `source_entry` are columns
planned for a future version (content locators, aliases, and explicit redirect/alias
typing). Naming one today is rejected with "not supported yet" rather than "unknown
column", so the deferral reads as a deferral rather than a typo.

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
| `CLUSTER_SIZE` | BIGINT | libzim default | target uncompressed cluster size in bytes |
| `WORKERS` | BIGINT | `4` | libzim's internal worker count |
| `ON_CONFLICT` | VARCHAR | `'error'` | duplicate-`path` policy: `'error'` or `'first'` |

A named metadata option (`TITLE`, `LANGUAGE`, …) and the equivalent `METADATA` key are the
same setting; supplying both for one key is a bind-time error rather than last-wins.

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
`csv`, which clobber by default. A ZIM is often the only copy of a corpus that took hours
to build, and a failed write can still leave an archive that opens and passes `zim_check()`
(the format has no way to record "this is incomplete"), so silently clobbering into a
partial result would be the worst combination. Remove the file yourself first if you mean
to replace it.

This also means a source archive can never be a valid `COPY` target — a query reading
`a.zim` can't accidentally truncate it by writing back to `'a.zim'`, because `a.zim`
already exists.

On any error partway through a write — a cast failure mid-stream, a duplicate path, an
invalid `MAIN_PATH` or redirect target — no file is left at the output path.

## Aliases are not preserved

An alias in a source archive shares its target's stored bytes, but `read_zim` can't tell an
alias apart from a normal item — same `is_redirect = false`, same content, same size. A
round trip through `COPY` therefore writes it back as a full duplicate item rather than an
alias: the archive stays semantically equivalent to a reader, but grows a second copy of
the data and its `zim_counter()` numbers change. A round-trip comparison that only checks
content will not catch this; compare `entry_count` / `all_entry_count` too.

## Not supported yet

- `content_path` (loading content from a local file or another archive lazily)
- `entry_kind` / `target` (explicit alias and redirect-kind columns)
- `PARTITION_BY` (one archive per key)

Each is rejected with a "not supported yet" error rather than being silently ignored.
