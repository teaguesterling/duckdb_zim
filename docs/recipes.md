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
WITH nhs_raw AS (                   -- parse each NHS page exactly once...
  SELECT regexp_extract(filename, 'medicines/([^/]+)/', 1)  AS medicine,
         html_extract_text(content::HTML, '//article//p')[1] AS nhs_says
  FROM read_text('zim://nhs.zim/www.nhs.uk/medicines/*/about-*/')
),
nhs AS (SELECT * FROM nhs_raw WHERE nhs_says IS NOT NULL),  -- ...then filter on the parsed value
merged AS (                         -- look the same name up in Wikipedia (one point lookup per row)
  SELECT medicine, nhs_says,
         zim_get_text('wikipedia_en_medicine.zim', upper(medicine[1]) || medicine[2:]) AS wiki_html
  FROM nhs
)
SELECT medicine, nhs_says,
       html_extract_text(wiki_html::HTML,
         '(//div[contains(@class,"mw-parser-output")]/p[normalize-space()])[1]')[1] AS wikipedia_says
FROM merged WHERE wiki_html IS NOT NULL;
```

This is a **key-lookup join**, not a scan-vs-scan join: NHS drives, and each row does a single
`zim_get_text` point lookup into Wikipedia by title — so the 362k-article archive is *probed*
~240 times, never scanned. ~5 s for the full table; the cost is HTML parsing, not libzim.

**Search both archives, then align the results side by side.** Federated `zim_search` queries
both in one call (the `file` column says which), and a `CASE` picks each archive's content
selector — so two independent search indexes are queried and matched in ~40 ms:

```sql
WITH hits AS (
  SELECT CASE WHEN file LIKE '%/nhs.zim' THEN 'NHS' ELSE 'Wikipedia' END AS source,
         html_extract_text(
           zim_get_text(file, path)::HTML,
           CASE WHEN file LIKE '%/nhs.zim'
                THEN '//article//p'
                ELSE '(//div[contains(@class,"mw-parser-output")]/p[normalize-space()])[1]' END
         )[1] AS lead
  FROM zim_search(['nhs.zim', 'wikipedia_en_medicine.zim'], 'warfarin blood clots', max_results := 1)
)
SELECT max(lead) FILTER (WHERE source = 'NHS')       AS nhs,
       max(lead) FILTER (WHERE source = 'Wikipedia') AS wikipedia
FROM hits;
```

Each corpus's own Xapian ranking picks the top hit, so this also shows what each encyclopedia
*considers* most relevant for the same query.

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

## Semantic search with a vector sidecar

ZIM carries its lexical index *inside* the archive (`X/fulltext/xapian`), but you cannot add
an index of your own there: libzim's `Creator` exposes only `configIndexing(true, lang)`, and
`getPath()` strips the namespace letter, so an `X/`-namespace blob is neither writable through
the public API nor nameable on the way back out.

A **sidecar** avoids that entirely and needs nothing from this extension. Keep embeddings in a
file next to the archive, keyed on the archive's own `path`, and let DuckDB do the vector work:

```sql
-- build once: <archive>.vec.parquet, keyed on path
COPY (
  SELECT path, embed(body) AS embedding      -- your embedding function
  FROM read_zim('corpus.zim', mimetype := 'text/html',
                include_content := true, content_as_varchar := true)
) TO 'corpus.zim.vec.parquet' (FORMAT parquet);
```

```sql
-- query: nearest neighbours, joined back for title/mimetype/content
SELECT v.path, z.title,
       array_cosine_similarity(v.embedding::FLOAT[1024], $q::FLOAT[1024]) AS sim
FROM read_parquet('corpus.zim.vec.parquet') v
JOIN read_zim('corpus.zim') z ON z.path = v.path
ORDER BY sim DESC LIMIT 10;
```

**Store unit vectors plus their scale factor.** Embedders do not necessarily hand you
normalized vectors — measured on a local Qwen3-Embedding-0.6B: **1024 dimensions**, L2 norms of
80.8, 95.6 and 97.4 for three different inputs, so not normalized and not a fixed scale either.
That matters because the metrics disagree on magnitude:

```sql
array_cosine_similarity([3,0,0], [1,0,0]) = 1.0    -- magnitude-invariant
array_inner_product(    [3,0,0], [1,0,0]) = 3.0    -- ranks partly by magnitude
```

Normalizing at build time and keeping the original magnitude in its own column settles it
without losing anything. The norm comes free from the array functions, since `|v| = sqrt(v·v)`:

```sql
COPY (
  SELECT path,
         sqrt(array_inner_product(e, e))::FLOAT AS norm,
         list_transform(e::FLOAT[], lambda x: x / sqrt(array_inner_product(e, e)))::FLOAT[1024]
           AS embedding
  FROM (SELECT path, embed(body)::FLOAT[1024] AS e FROM read_zim(…))
) TO 'corpus.zim.vec.parquet' (FORMAT parquet);
```

Three things that buys, all verified:

- **Either metric is now correct.** On unit vectors `array_inner_product` equals
  `array_cosine_similarity` exactly, and `ip` is the cheaper of the two (no division per
  comparison). It also makes `vss`'s HNSW `ip` metric safe to use, which assumes unit vectors.
- **Nothing is lost.** `list_transform(embedding::FLOAT[], lambda x: x * norm)` returns the
  original vector exactly, so the sidecar is a lossless store, not a lossy one.
- **The norm is useful on its own** — it carries magnitude information the unit vector throws
  away, which is sometimes a usable signal (and a cheap sanity check: a norm far from the
  corpus distribution usually means a degenerate or empty input).

Use the `lambda x:` form rather than the old `->` arrow, which DuckDB now warns is deprecated.

**Assert the row count when you build it.** The `COPY (SELECT path, embed(body) …)` form
above zips paths to vectors positionally, which is only safe if the embedder returns exactly
one vector per input. Not all do. A batch embedder that silently drops blank or
whitespace-only inputs returns N-1 vectors for N rows, and positional zipping then
**misaligns every entry after the drop** — producing an index that is consistent, queryable
and wrong, with no error anywhere. Observed on one local embedder
(Qwen3-Embedding-0.6B) through a file-batch CLI path: a blank line mid-batch, a
whitespace-only line, or a trailing blank line each yield 3 lines -> 2 vectors. **The exact
invocation is not recorded**, so treat this as an observed hazard rather than a reproducible
recipe — the command string first written down for it does not exist on that device. The
hazard is also **path-specific**: the same device's HTTP
`/v1/embeddings` array endpoint returned 3 vectors for `["a","","c"]`, each carrying an explicit
`index`. Prefer an API that returns a per-input index and correlate on that, so a drop becomes
impossible to misread; otherwise compare the returned count against the input count and refuse
on mismatch, or correlate by `path` rather than by position.

**Do not use vector equality to check a rebuild.** Embedders are not necessarily bit-exact:
the same model on the same device, same text, same batch, measured sporadic per-call
deviations of <= ~2.5e-3 on ~1-2% of inputs, with the affected rows varying between calls. So
"re-embed and compare" needs a **pre-registered floor** (take the maximum repeat distance over
a sample, multiply by a stated factor, fix it before looking at the value you care about), not
an equality test. This is not a real obstacle to the check that matters: a genuine misalignment
reads as order 1 against a ~1e-3 noise floor, three orders of margin. And for content dedup,
hash the **text**, never the vector — text hashing is exact and vector comparison never is.

**Cast the embedding on read.** Parquet does not preserve DuckDB's fixed-size `ARRAY` type: a
`FLOAT[1024]` column comes back as `FLOAT[]`, and `array_cosine_similarity` has no overload for
a mixed `FLOAT[]` / `FLOAT[1024]` pair, so it fails to bind without the `::FLOAT[1024]`.

**Hybrid ranking** falls out, because both sides are just relations — Xapian for lexical,
the sidecar for semantic, combined however you like (scores are per-archive, so normalize
before mixing):

```sql
SELECT s.path, s.score AS lexical,
       array_cosine_similarity(v.embedding::FLOAT[1024], $q::FLOAT[1024]) AS semantic
FROM zim_search('corpus.zim', 'photosynthesis') s
LEFT JOIN read_parquet('corpus.zim.vec.parquet') v ON v.path = s.path;
```

**Globs and remote archives**: derive each sidecar from the archive it belongs to rather than
hardcoding one path — `include_filepath := true` adds the `file_path` column, so
`file_path || '.vec.parquet'` names the right sidecar per archive across a glob or an S3 prefix.

**Why a sidecar rather than embedding the index in the archive.** Both are possible — a vector
index *can* be written into a ZIM as an ordinary content item (`compress := false` keeps the
dense bytes uncompressed, `front_article := false` keeps it out of article listings, and a
declaring mimetype makes it discoverable via `zim_counter()` without reading it). But the
sidecar wins on three counts: the archive stays a valid stock ZIM that Kiwix can read; you can
re-embed with a better model without rewriting gigabytes; and DuckDB already supplies the
vector primitives, so nothing here needs to link a vector library. Embedding it inside is the
right call only when the archive must travel as a single self-contained file — and then it
should follow the pattern Xapian already uses over remote storage: one contiguous uncompressed
blob, copied out through the byte-range reader to a temp file and opened from there, capped by
a setting (`zim_remote_search_max_local_index` is the precedent).

**If you expect to move in-archive later, design the sidecar as the blob you would embed.**
Keep it one self-contained file, uncompressed, with the dimension and model identity recorded
inside it rather than in the filename. Then the migration is "write these same bytes as an
entry" on the way in, and "extract to a temp file, open from there" on the way out — the Xapian
path again. Nothing about the query layer changes, because `read_parquet` wants a path either
way. Note that upstream offers no sanctioned mechanism here: the ZIM index format is
deliberately undocumented (openzim/overview#6 — the maintainer declines to specify it in order
to "allow the ZIM format to support other kinds of fulltext indexes"), and libzim2's plan hides
namespaces further behind `addMetadata`/`addRedirect`/`addEntry`. So an in-archive index is a
private convention, not an interoperable one — which is a reason to keep the sidecar form
usable on its own, not merely a staging step.

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
