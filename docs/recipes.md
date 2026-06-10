# Recipes

ZIM articles are **HTML** (mwoffliner / zimwriterfs / zimit all emit `text/html`), so the
natural partner is [`webbed`](https://github.com/teaguesterling/duckdb_webbed). `duckdb_zim`
deliberately knows nothing about HTML — it hands you bytes and lets `webbed` own parsing.

!!! note "Article-body selectors are scraper-specific"
    Raw article HTML is wrapped in nav / header / footer chrome, so extract the body with
    an XPath onto the content container, not `//body`. mwoffliner wikis use
    `//div[contains(@class,"mw-parser-output")]`; zimit captures keep the site's own
    markup, often `//main` or `//article`. The exact selector varies by scraper version.

## Pipe article HTML into webbed

```sql
-- requires: LOAD webbed;
-- include_content := 'text/html' decompresses only the article HTML
SELECT path,
       html_extract_text(content::HTML, '//h1')[1] AS heading,
       html_extract_links(content::HTML)           AS links
FROM read_zim('wikipedia.zim', include_content := 'text/html', content_as_varchar := true)
WHERE mimetype = 'text/html';
```

## Build your own full-text index

Useful for archives without a Xapian index, or when you want your own ranking:

```sql
CREATE TABLE corpus AS
SELECT path, title,
       html_extract_text(content::HTML, '//div[contains(@class,"mw-parser-output")]')[1] AS body
FROM read_zim('wikimed.zim', mimetype := 'text/html',
              include_content := true, content_as_varchar := true);

PRAGMA create_fts_index('corpus', 'path', 'title', 'body');
```

## Corpus-wide link graph

`html_extract_links(content)` gives hrefs; internal links are entry paths, so normalize and
join back to the entries table to build an edge list, then run graph algorithms with
recursive CTEs.

## HTML → clean markdown → local LLM

`html_to_duck_blocks(content)` (webbed + `duck_block_utils`) yields a structured block tree
you can render to markdown as retrieval context for an offline model. ZIM = corpus,
webbed + duck_block_utils = cleaner, DuckDB = retrieval.

## Web-capture (zimit) archives

zimit / browsertrix ZIMs use full-URL content paths (`www.nhs.uk/medicines/insulin/`)
instead of mwoffliner's `A/Foo`. The extension stays scraper-agnostic — paths are opaque
strings — so everything is plain SQL on the `path` column, no special functions:

```sql
-- which scraper produced this archive?
SELECT zim_metadata('archive.zim', 'Scraper');          -- 'zimit …' vs 'mwoffliner …'

-- hosts captured, and entries under one host with reconstructed source URLs
SELECT DISTINCT split_part(path, '/', 1) AS host FROM read_zim('archive.zim');
SELECT path, 'https://' || path AS source_url
FROM read_zim('archive.zim')
WHERE path LIKE 'www.nhs.uk/%' AND mimetype = 'text/html';

-- read a captured page straight through the filesystem (trailing slashes resolve)
SELECT * FROM read_html('zim://archive.zim/www.nhs.uk/medicines/insulin/');   -- LOAD webbed;
```
