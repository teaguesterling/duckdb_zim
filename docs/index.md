# duckdb_zim

Read **`.zim` files** — the [openZIM](https://openzim.org) archive format served by
[Kiwix](https://kiwix.org): offline Wikipedia, WikiMed, Wiktionary, Stack Exchange,
iFixit, Project Gutenberg, and hundreds of other libraries — directly in DuckDB via
[libzim](https://github.com/openzim/libzim).

A ZIM packages an entire website into a single compressed, content-addressed file.
`duckdb_zim` turns any such archive into a SQL-queryable table — or a queryable
filesystem — without unpacking it.

```sql
INSTALL zim FROM community;
LOAD zim;

-- list every content entry (no content fetched — fast even on huge archives)
SELECT path, title, mimetype FROM read_zim('wikipedia.zim') LIMIT 10;

-- full-text search the archive's Xapian index
SELECT path, title, snippet FROM zim_search('wikipedia.zim', 'photosynthesis');

-- compose with webbed over the zim:// filesystem
SELECT html_extract_text(content::HTML, '//h1')[1] AS heading
FROM read_text('zim://wikipedia.zim/A/Photosynthesis');   -- LOAD webbed;
```

## What it does

- **[Read archives](reading.md)** — `read_zim()` yields one row per content entry with
  projection pushdown and lazy content; exact lookup, prefix listing, Accept-style
  `mimetype` matching, glob / multi-file, and the `FROM 'x.zim'` replacement scan.
- **[The `zim://` filesystem](filesystem.md)** — address an entry inside an archive from
  any path-reading function (`read_text` / `read_blob`, or `webbed`'s `read_html`), with
  zero coupling.
- **[Search & suggestions](search.md)** — `zim_search` (Xapian full-text) and
  `zim_suggest` (title autocomplete), federated across many archives.
- **Metadata & utilities** — `read_zim_metadata`, `zim_counter`, `zim_info`,
  `zim_illustration`, `zim_random`, `zim_check`, and the lookup scalars. See the
  **[function reference](reference.md)**.

## Status

**v0.2.0 — feature-complete.** Linux + macOS (x64 + arm64) build with full-text search;
the **WebAssembly** build is green and search-less. Windows is not yet supported.

!!! note "License: GPL-2.0-or-later"
    libzim is GPL, so this extension is too — intentional, and separate from the
    MIT-licensed `markdown` / `yaml` / `webbed` family. See [Design notes](design.md).
