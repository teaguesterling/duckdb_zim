# Recipes

ZIM articles are **HTML** (mwoffliner / zimwriterfs / zimit all emit `text/html`), so the
natural partner is [`webbed`](https://github.com/teaguesterling/duckdb_webbed). `duckdb_zim`
deliberately knows nothing about HTML — it hands you bytes and lets `webbed` own parsing.

!!! note "Article-body selectors are scraper-specific"
    Raw article HTML is wrapped in nav / header / footer chrome, so extract the body with
    an XPath onto the content container, not `//body`. mwoffliner wikis use
    `//div[contains(@class,"mw-parser-output")]`; zimit captures keep the site's own
    markup, often `//main` or `//article`. The exact selector varies by scraper version.

## Query offline Wikipedia with SQL + XPath

The headline use case: an entire offline encyclopedia becomes a queryable, structured
dataset — no scraping, no unpacking, no internet. `zim` hands `webbed` the article HTML
over the `zim://` seam, and XPath does the rest.

**Decompose one article.** Wikipedia (mwoffliner) wraps content in `mw-parser-output`:

```sql
-- requires: LOAD webbed;
WITH a AS (SELECT zim_get_text('wikipedia_en_medicine.zim', 'Aspirin')::HTML AS doc)
SELECT html_extract_text(doc, '//h1')[1]                                          AS title,
       html_extract_text(doc, '//div[contains(@class,"mw-parser-output")]//h2')   AS sections,
       len(html_extract_links(doc))                                               AS links
FROM a;
-- Aspirin · [Medical uses, Adverse effects, Pharmacology, …] · 2601
```

**Full-text search the corpus, then XPath-extract each hit:**

```sql
SELECT path,
       html_extract_text(zim_get_text('wikipedia_en_medicine.zim', path)::HTML,
         '(//div[contains(@class,"mw-parser-output")]/p[normalize-space()])[1]')[1] AS lead
FROM zim_search('wikipedia_en_medicine.zim', 'nonsteroidal anti-inflammatory', max_results := 5);
```

**Join two different offline encyclopedias on a shared key.** Here the NHS website (a zimit
capture, content in `//article`) and Wikipedia (mwoffliner, content in `//div[mw-parser-output]`)
are cross-referenced on the drug name — two different HTML conventions, reconciled by XPath
in one query:

```sql
WITH nhs AS (                       -- offline NHS site
  SELECT regexp_extract(filename, 'medicines/([^/]+)/', 1)  AS medicine,
         html_extract_text(content::HTML, '//article//p')[1] AS nhs_says
  FROM read_text('zim://nhs.zim/www.nhs.uk/medicines/*/about-*/')
  WHERE html_extract_text(content::HTML, '//article//p')[1] IS NOT NULL   -- parse is the filter
),
merged AS (                         -- look the same name up in Wikipedia
  SELECT medicine, nhs_says,
         zim_get_text('wikipedia_en_medicine.zim', upper(medicine[1]) || medicine[2:]) AS wiki_html
  FROM nhs
)
SELECT medicine, nhs_says,
       html_extract_text(wiki_html::HTML,
         '(//div[contains(@class,"mw-parser-output")]/p[normalize-space()])[1]')[1] AS wikipedia_says
FROM merged WHERE wiki_html IS NOT NULL;
```

!!! tip "Filter on the parse, not the string"
    To skip pages that aren't real articles, don't substring-match the raw HTML
    (`content LIKE '%<article%'`). `html_extract_text` returns an **empty array** when the
    XPath matches nothing, so the structural predicate is `… IS NOT NULL` (or
    `len(html_extract_text(...)) > 0`) on the extracted value — computed once in a CTE.

!!! note "Composing with other extensions"
    `zim` ships no HTML knowledge on purpose — it hands you the raw bytes and lets a
    parsing extension do the work. **Anything** that reads through DuckDB's file layer
    composes over the `zim://` seam: `webbed` for HTML/XPath here, but equally `read_json`,
    `read_csv`, `read_text`, or `read_blob` for other payloads inside an archive. The
    extension stays a narrow "read the container" tool; everything rich lives in the
    extensions you already use.

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
