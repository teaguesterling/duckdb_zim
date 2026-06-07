# duckdb_zim

Read **`.zim` files** (Kiwix / openZIM archives — offline Wikipedia, WikiMed, Stack
Exchange, iFixit, Project Gutenberg, …) directly in DuckDB via [libzim](https://github.com/openzim/libzim).

Query an archive's entries, pull article content, read metadata, and compose with the
rest of the DuckDB extension ecosystem (notably [`webbed`](https://github.com/teaguesterling/duckdb_webbed)
for the HTML that ZIM articles are made of).

> **Status: phase 1.** Content scan, lookups, listing, and metadata are implemented.
> The `zim://` filesystem, full-text search, and `ATTACH` are planned (see
> [Roadmap](#roadmap)). This is a young extension — expect rough edges.

> **License: GPL-2.0-or-later.** libzim is GPL, so this extension is too. That is
> intentional and separate from the MIT-licensed `markdown`/`yaml`/`webbed` family;
> see [License](#license).

> **Platforms: native (Linux / macOS / Windows).** A WebAssembly build is an open
> question gated on libzim's search dependency; see [Platforms](#platforms).

---

## Install

Not yet published to community-extensions. Build from source (see `HANDOFF.md` /
`DESIGN.md` for the toolchain), then:

```sql
LOAD zim;
```

Once published, the usual:

```sql
INSTALL zim FROM community;
LOAD zim;
```

---

## Quick start

```sql
-- list every content entry (no content fetched — fast even on huge archives)
SELECT path, title, mimetype, size
FROM read_zim('wikipedia.zim')
ORDER BY path;

-- what kind of archive is this? counts and flags
SELECT zim_info('wikipedia.zim');

-- what's actually in it? the archive's self-describing mimetype histogram
SELECT zim_counter('wikipedia.zim');     -- MAP('text/html' -> 11604, 'image/webp' -> 987, ...)

-- pull one article's HTML
SELECT zim_get_text('wikipedia.zim', 'A/Photosynthesis');
```

A note on **paths**: under the current ZIM namespace scheme, content paths are
namespace-free (`A/Photosynthesis`, not `C/A/Photosynthesis`). The `A/`, `I/` prefixes
you see are path text from the scraper, not libzim namespaces. A leading `C/` is
tolerated on lookup and normalized away. See `docs/libzim-semantics.md` for the verified
model.

---

## Core usage

### Scanning and listing

`read_zim` yields one row per content entry. Content is **lazy** — it is `NULL` unless
you both project the column and pass `include_content := true`, so a listing scan never
decompresses the archive.

```sql
-- prefix listing (efficient: backed by libzim findByPath, not a full scan)
SELECT path, title FROM read_zim('wikipedia.zim', path_prefix := 'A/Cal');

-- prefix on title, over the title listing (autocomplete-style)
SELECT title FROM read_zim('wikipedia.zim', title_prefix := 'Calc', listing := 'title');

-- filter by mimetype (excludes redirects, which have no mimetype)
SELECT path, size FROM read_zim('wikipedia.zim', mimetype := 'text/css');

-- exact lookup by path or title (emits 0 or 1 row)
SELECT * FROM read_zim('wikipedia.zim', path := 'A/Photosynthesis');

-- fetch content (opt in). Default: BLOB. content_as_varchar := true makes the column
-- VARCHAR; entries whose bytes aren't valid UTF-8 (binary: images, fonts, …) come back
-- NULL rather than mangled — same "never mangle" rule as zim_get_text.
SELECT path, content
FROM read_zim('wikipedia.zim', path := 'A/Photosynthesis',
              include_content := true, content_as_varchar := true);
```

`read_zim` parameters: `include_content`, `content_as_varchar`, `include_filepath`
(alias `filename`), `mimetype`, `path`, `title`, `path_prefix`, `title_prefix`,
`listing` (`'path'` | `'title'`).

> **`listing` (and `title_prefix`) are articles-only.** `listing := 'title'` selects
> libzim's *title listing* — its `iterByTitle`, which contains only entries marked
> `FRONT_ARTICLE` (the articles), not every entry. So it is a different row set, not
> just a re-ordering of the default `'path'` listing (which is all entries).
> `title_prefix` rides the same title listing. For *all* entries in title order, scan
> the default path listing and `ORDER BY title` in SQL.

Columns: `path, title, mimetype, is_redirect, redirect_path, size, content[, file_path]`.

### Metadata

Metadata is a separate key space from content (two different libzim doors). Both a table
function and direct scalar lookups are provided.

```sql
SELECT key, value FROM read_zim_metadata('wikipedia.zim');

SELECT zim_metadata('wikipedia.zim', 'Title');        -- single value
SELECT zim_metadata('wikipedia.zim', 'Language');     -- ISO 639-3, may be comma-list
SELECT zim_metadata_keys('wikipedia.zim');            -- LIST(VARCHAR)
SELECT zim_counter('wikipedia.zim');                  -- MAP(mimetype -> count)
SELECT zim_info('wikipedia.zim');                     -- STRUCT of counts/flags/uuid
```

### Scalar helpers

```sql
zim_get_content(file, path)     -- BLOB, follows redirects
zim_get_text(file, path)        -- VARCHAR for text mimetypes; NULL for binary (never mangled)
zim_has_entry(file, path)       -- BOOLEAN
zim_redirect_target(file, path) -- VARCHAR, NULL if not a redirect
zim_mimetype(file, path)        -- VARCHAR
zim_main_entry(file)            -- VARCHAR, redirect-resolved landing path
```

---

## Integration with other extensions

ZIM articles are **HTML** (mwoffliner / zimwriterfs / zimit all emit `text/html`), so the
natural partner is `webbed`. `duckdb_zim` deliberately knows nothing about HTML — it hands
you bytes and lets `webbed` own parsing. This keeps the GPL surface confined to "read the
container" and leaves all rich processing in the MIT ecosystem.

### Value seam (works today)

Pass article HTML straight into `webbed`'s extractors:

```sql
-- requires: LOAD webbed;
SELECT path,
       html_extract_text(content, '//h1')[1] AS heading,
       html_extract_links(content)           AS links
FROM read_zim('wikipedia.zim', include_content := true, content_as_varchar := true)
WHERE mimetype = 'text/html';
```

> **mwoffliner chrome:** raw article HTML is wrapped in nav / infobox / footer markup.
> Extract the article body with an XPath onto the content container (commonly
> `//div[contains(@class,"mw-parser-output")]`), not `//body` — the exact selector
> varies by scraper version and skin.

### Filesystem seam (planned — phase 2)

Once the `zim://` filesystem lands, any path-reading extension composes for free:

```sql
-- PLANNED, not yet implemented:
SELECT * FROM read_html('zim://wikipedia.zim/A/Photosynthesis');
SELECT * FROM read_html('zim://wikipedia.zim/A/*');     -- via the zim:// glob
SELECT * FROM read_blob('zim://wikipedia.zim/I/logo.webp');
```

### Recipes

**Build your own full-text index** (useful for archives without a Xapian index, or when
you want your own ranking):

```sql
CREATE TABLE corpus AS
SELECT path, title,
       html_extract_text(content, '//div[contains(@class,"mw-parser-output")]')[1] AS body
FROM read_zim('wikimed.zim', include_content := true, content_as_varchar := true)
WHERE mimetype = 'text/html';

PRAGMA create_fts_index('corpus', 'path', 'title', 'body');
```

**Corpus-wide link graph** — `html_extract_links` gives hrefs; internal links are entry
paths, so normalize and join back to the entries table to build an edge list.

**HTML → clean markdown → local LLM** — `html_to_duck_blocks(content)` (webbed +
`duck_block_utils`) yields a structured block tree you can render to markdown as retrieval
context for an offline model.

---

## Platforms

Native Linux / macOS / Windows. libzim brings `zstd`, `liblzma`, `icu`, and optionally
`xapian` (search). Those individually compile to WebAssembly (DuckDB-WASM already ships
`zstd` and `icu`), so a search-less WASM build is a plausible spike — gated on (a) whether
libzim drops `icu` when built without `xapian`, and (b) whether libzim's own build works
under emscripten. Search is a compile-time option for exactly this reason.

---

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | `read_zim`, listing/prefix, `read_zim_metadata`, `zim_metadata`/`_keys`/`zim_counter`/`zim_info`, lookup scalars | **implemented** |
| 2 | `zim://` filesystem (path + glob) — composition with `webbed`/`markdown`/`read_blob` | planned |
| 3 | `zim_search` / `zim_suggest` (Xapian full-text + suggestion index) | planned |
| 4 | `ATTACH 'x.zim' AS … (TYPE zim)` — read-only catalog ergonomics | planned |

---

## How it works

A process-wide pool keeps each opened `zim::Archive` alive across queries, so libzim's
decompressed-cluster cache stays warm — every function (and the future filesystem and
`ATTACH`) shares one open handle per file. All libzim contact is isolated in a small
access layer (`src/zim_access.*`); the rest of the code is a plain DuckDB binding over
DuckDB-agnostic structs. See `DESIGN.md` and `docs/libzim-semantics.md`.

---

## License

**GPL-2.0-or-later**, inherited from libzim. Distributing a build that statically links
libzim makes the combined work GPL. If you need a permissively licensed ZIM reader, this
is not it — but it is free to use, modify, and redistribute under the GPL terms.
