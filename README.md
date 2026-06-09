# duckdb_zim

Read **`.zim` files** (Kiwix / openZIM archives — offline Wikipedia, WikiMed, Stack
Exchange, iFixit, Project Gutenberg, …) directly in DuckDB via [libzim](https://github.com/openzim/libzim).

Query an archive's entries, pull article content, read metadata, and compose with the
rest of the DuckDB extension ecosystem (notably [`webbed`](https://github.com/teaguesterling/duckdb_webbed)
for the HTML that ZIM articles are made of).

> **Status: phases 1–3.** Content scan, lookups, listing, metadata, the `zim://`
> filesystem, and Xapian full-text search (`zim_search`, native builds) are
> implemented. `ATTACH` is still planned (see [Roadmap](#roadmap)). This is a young
> extension — expect rough edges.

> **License: GPL-2.0-or-later.** libzim is GPL, so this extension is too. That is
> intentional and separate from the MIT-licensed `markdown`/`yaml`/`webbed` family;
> see [License](#license).

> **Platforms: Linux and macOS** (x64 + arm64, with full-text search) **and WebAssembly**
> (search-less) all build and test green in CI. Windows isn't supported yet; see
> [Platforms](#platforms).

---

## Install

A community-extensions submission is in progress. Once it lands, the usual:

```sql
INSTALL zim FROM community;
LOAD zim;
```

Until then, build from source (DuckDB + extension are pulled as submodules):

```sh
git clone --recurse-submodules https://github.com/teaguesterling/duckdb_zim.git
cd duckdb_zim
make            # needs a vcpkg toolchain for libzim; see DESIGN.md for details
```

then load the built extension:

```sql
LOAD 'build/release/extension/zim/zim.duckdb_extension';
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

A note on **paths**: content paths are namespace-free (`A/Photosynthesis`, not
`C/A/Photosynthesis`) and their shape is **scraper-dependent** — the prefixes are path
text, not libzim namespaces. mwoffliner emits `A/Photosynthesis`, `I/logo.png`; zimit /
browsertrix emits full URLs with a trailing slash, e.g. `www.nhs.uk/medicines/insulin/`.
The extension treats them as opaque content paths either way; a leading `C/` is tolerated
on lookup and normalized away. See `docs/libzim-semantics.md` for the verified model.

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

-- bulk content, but only for the mimetypes you want: include_content also accepts a
-- mimetype or a list. Matching entries are decompressed; everything else keeps its
-- metadata row with NULL content — the archive's images/fonts are never materialized.
SELECT path, content
FROM read_zim('wikipedia.zim', include_content := 'text/html');
SELECT path, content
FROM read_zim('wikipedia.zim', include_content := ['text/html', 'text/css']);
```

`read_zim` parameters: `include_content` (`true`/`false`, or a mimetype / list of
mimetypes to load content for only those entries), `content_as_varchar`,
`include_filepath` (alias `filename`), `mimetype`, `path`, `title`, `path_prefix`,
`title_prefix`, `listing` (`'path'` | `'title'`).

These select mutually exclusive modes and conflicting combinations are rejected (rather
than silently resolved): pick at most one exact key (`path` *or* `title`) **or** one
prefix (`path_prefix` *or* `title_prefix`); an exact key can't be combined with a
prefix/`listing`; and `listing` must not contradict a prefix's order (`title_prefix` +
`listing := 'title'` is fine, `path_prefix` + `listing := 'title'` is an error).
`mimetype` composes with any of them.

> **`listing` (and `title_prefix`) are articles-only.** `listing := 'title'` selects
> libzim's *title listing* — its `iterByTitle`, which contains only entries marked
> `FRONT_ARTICLE` (the articles), not every entry. So it is a different row set, not
> just a re-ordering of the default `'path'` listing (which is all entries).
> `title_prefix` rides the same title listing. For *all* entries in title order, scan
> the default path listing and `ORDER BY title` in SQL.

Columns: `path, title, mimetype, is_redirect, redirect_path, size, content[, file_path]`.

### Multiple archives, globs, and `FROM 'x.zim'`

`read_zim` accepts one path, a glob, or a list — rows from each archive are concatenated,
and `include_filepath := true` tells them apart. A bare `.zim` filename in `FROM` is
rewritten to `read_zim` automatically.

```sql
SELECT count(*) FROM read_zim('wikis/*.zim');                       -- glob
SELECT * FROM read_zim(['a.zim', 'b.zim'], include_filepath := true); -- explicit list
SELECT * FROM 'wikipedia.zim';                                      -- replacement scan
```

Exact lookups and prefixes apply **per archive**: `read_zim('*.zim', path := 'A/Foo')`
emits one row for each archive that contains `A/Foo`.

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

### Full-text search

If the archive carries a Xapian full-text index (`zim_info(file).has_fulltext_index`),
`zim_search` queries it:

```sql
SELECT path, title, score, snippet
FROM zim_search('wikipedia.zim', 'compound heterozygous', max_results := 20);

-- paginate with result_offset; both default to (25, 0)
SELECT path FROM zim_search('wikipedia.zim', 'photosynthesis',
                            max_results := 10, result_offset := 10);
```

Returns `(path, title, score DOUBLE, snippet VARCHAR)` ranked by relevance. `score` is the
Xapian rank; `snippet` is a best-effort highlighted excerpt (`NULL` when none is produced).
An archive without a full-text index — or the search-less **WebAssembly** build — returns
no rows rather than erroring. (`max_results` / `result_offset`, not `limit` / `offset`,
because the latter are SQL reserved words.)

### Reading a real archive, end to end

Walking a real zimit archive — `nhs.uk_en_medicines_2025-12.zim` (~2k entries):

```sql
-- what is this? counts/flags, the human title, and the self-describing mimetype histogram
SELECT zim_info('nhs.uk_en_medicines_2025-12.zim');               -- entry_count 2064, uuid, …
SELECT zim_metadata('nhs.uk_en_medicines_2025-12.zim', 'Title');  -- NHS' Medicines A to Z
SELECT zim_counter('nhs.uk_en_medicines_2025-12.zim');            -- text/html→1996, image/jpeg→9, …

-- the landing page (redirect-resolved)
SELECT zim_main_entry('nhs.uk_en_medicines_2025-12.zim');         -- www.nhs.uk/medicines/

-- browse the biggest articles — no content fetched, so it's fast
SELECT path, title, size
FROM read_zim('nhs.uk_en_medicines_2025-12.zim', mimetype := 'text/html')
ORDER BY size DESC LIMIT 10;                                      -- Insulin, HRT, Propranolol, …

-- read one article's HTML as text
SELECT zim_get_text('nhs.uk_en_medicines_2025-12.zim', 'www.nhs.uk/medicines/insulin/');
```

> **Heads up on identifiers:** the archive's `Name` metadata (`nhs.uk_en_medicines`) is
> *not* the on-disk filename stem (`nhs.uk_en_medicines_2025-12`) — the date lives only in
> the filename, and kiwix serves content by the filename stem. Don't build content URLs
> from `Name`.

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
-- include_content := 'text/html' loads (and decompresses) only the article HTML —
-- the archive's images and fonts are skipped, not just filtered out afterwards.
SELECT path,
       html_extract_text(content, '//h1')[1] AS heading,
       html_extract_links(content)           AS links
FROM read_zim('wikipedia.zim', include_content := 'text/html', content_as_varchar := true)
WHERE mimetype = 'text/html';
```

> **Article-body selectors are scraper-specific:** raw article HTML is wrapped in
> nav / header / footer chrome, so extract the body with an XPath onto the content
> container, not `//body`. mwoffliner wikis use `//div[contains(@class,"mw-parser-output")]`;
> zimit / browsertrix captures (like the NHS archive above) keep the site's own markup,
> often `//main` or `//article`. The exact selector varies by scraper version and skin.

### Filesystem seam (`zim://`)

The extension registers a read-only `zim://` filesystem, so **any** path-reading function
— DuckDB's own `read_text` / `read_blob`, or `webbed`'s `read_html`, or anything else that
goes through DuckDB's file layer — composes over ZIM contents for free, with no coupling
and no GPL linkage back into those extensions:

```sql
SELECT * FROM read_text('zim://wikipedia.zim/A/Photosynthesis');   -- one entry as text
SELECT * FROM read_blob('zim://wikipedia.zim/I/logo.webp');        -- one entry as bytes
SELECT * FROM read_html('zim://wikipedia.zim/A/Photosynthesis');   -- requires: LOAD webbed;
SELECT * FROM read_text('zim://wikipedia.zim/A/C*');               -- glob over content paths
```

The grammar is **content-path-first**: `zim://<archive>.zim/<content-path>`. The archive
component is a local file (libzim mmaps it directly); everything after the `.zim/` boundary
is the entry path within the archive. A leading `C/` on the content path is tolerated and
stripped, redirects are followed (a redirect path serves its target's bytes, like a
symlink), and `*` / `?` / `[…]` globs are matched against entry paths. Entries are
materialized into memory on open — fine for articles, but a single very large media item
costs its size in RAM.

```sql
-- extract every article's <h1> by globbing the filesystem and piping to webbed:
SELECT regexp_replace(filename, '^.*/', '') AS entry,
       html_extract_text(content, '//h1')[1] AS heading
FROM read_text('zim://wikipedia.zim/A/*')
ORDER BY entry;   -- requires: LOAD webbed;
```

### Recipes

**Build your own full-text index** (useful for archives without a Xapian index, or when
you want your own ranking):

```sql
CREATE TABLE corpus AS
SELECT path, title,
       html_extract_text(content::HTML, '//div[contains(@class,"mw-parser-output")]')[1] AS body
FROM read_zim('wikimed.zim', mimetype := 'text/html',
              include_content := true, content_as_varchar := true);

PRAGMA create_fts_index('corpus', 'path', 'title', 'body');
```

**Corpus-wide link graph** — `html_extract_links` gives hrefs; internal links are entry
paths, so normalize and join back to the entries table to build an edge list.

**HTML → clean markdown → local LLM** — `html_to_duck_blocks(content)` (webbed +
`duck_block_utils`) yields a structured block tree you can render to markdown as retrieval
context for an offline model.

---

## Platforms

**Linux** (x64 + arm64) and **macOS** (x64 + arm64) build and test green in CI, **with
full-text search** — `xapian` is pulled in for native triplets. The **WebAssembly** build
works too (all three emscripten variants compile green), but is **search-less**: `xapian`
is gated out of the Wasm triplet (`"platform": "!emscripten"` in `vcpkg.json`), since
libzim + `icu` + `zstd` + `liblzma` build under emscripten but xapian does not. On Wasm,
`zim_search` binds but returns no rows.

**Windows** is not yet supported — libzim + icu under MSVC/mingw need their own porting
work, so it's excluded from CI for now.

---

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | `read_zim`, listing/prefix, `read_zim_metadata`, `zim_metadata`/`_keys`/`zim_counter`/`zim_info`, lookup scalars | **implemented** |
| 2 | `zim://` filesystem (path + glob) — composition with `webbed`/`markdown`/`read_blob` | **implemented** |
| 3 | `zim_search` (Xapian full-text, native builds) — `zim_suggest` (suggestion index) still to come | **implemented** |
| 4 | `ATTACH 'x.zim' AS … (TYPE zim)` — read-only catalog ergonomics | planned |

---

## How it works

A process-wide pool keeps each opened `zim::Archive` alive across queries, so libzim's
decompressed-cluster cache stays warm — every function, the `zim://` filesystem, and a
future `ATTACH` all share one open handle per file. All libzim contact is isolated in a small
access layer (`src/zim_access.*`); the rest of the code is a plain DuckDB binding over
DuckDB-agnostic structs. See `DESIGN.md` and `docs/libzim-semantics.md`.

---

## License

**GPL-2.0-or-later**, inherited from libzim. Distributing a build that statically links
libzim makes the combined work GPL. If you need a permissively licensed ZIM reader, this
is not it — but it is free to use, modify, and redistribute under the GPL terms.
