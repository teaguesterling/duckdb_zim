# Reading archives

`read_zim()` yields one row per content entry. Content is **lazy** — `content` is `NULL`
unless you both project the column and opt in with `include_content`, so a listing scan
never decompresses the archive.

Columns: `path, title, mimetype, is_redirect, redirect_path, size, content[, file_path]`.

```sql
SELECT path, title, mimetype, size
FROM read_zim('wikipedia.zim')
ORDER BY path;
```

!!! note "Paths are scraper-dependent"
    Content paths are namespace-free and their shape depends on the scraper. mwoffliner
    emits `A/Photosynthesis`, `I/logo.png`; zimit / browsertrix emit full URLs with a
    trailing slash, e.g. `www.nhs.uk/medicines/insulin/`. The extension treats them as
    opaque content paths either way; a leading `C/` is tolerated on lookup and normalized
    away. See [libzim semantics](libzim-semantics.md).

## Listing, prefixes, and lookup

```sql
-- prefix listing (efficient: backed by libzim findByPath, not a full scan)
SELECT path, title FROM read_zim('wikipedia.zim', path_prefix := 'A/Cal');

-- prefix on title, over the title listing (autocomplete-style)
SELECT title FROM read_zim('wikipedia.zim', title_prefix := 'Calc', listing := 'title');

-- exact lookup by path or title (emits 0 or 1 row)
SELECT * FROM read_zim('wikipedia.zim', path := 'A/Photosynthesis');
```

!!! warning "`listing := 'title'` is articles-only"
    It selects libzim's *title listing* (`iterByTitle`), which contains only entries
    marked `FRONT_ARTICLE` — a different row set, not just a re-ordering of the default
    `'path'` listing. `title_prefix` rides the same title listing. For all entries in
    title order, scan the default listing and `ORDER BY title` in SQL.

These parameters select mutually exclusive modes; conflicting combinations are **rejected**
rather than silently resolved (e.g. `path` + `title`, two prefixes, or `path_prefix` +
`listing := 'title'`).

## Content

```sql
-- fetch content (opt in). Default BLOB; content_as_varchar makes it VARCHAR.
-- Non-UTF-8 bytes come back NULL rather than mangled.
SELECT path, content
FROM read_zim('wikipedia.zim', path := 'A/Photosynthesis',
              include_content := true, content_as_varchar := true);
```

`include_content` also accepts a **mimetype or list of mimetypes** (with `image/*` / `*/*`
wildcards). Matching entries are decompressed; everything else keeps its metadata row with
`NULL` content — pushdown, not a post-filter, so the archive's images and fonts are never
materialized when you only want the HTML:

```sql
SELECT path, content FROM read_zim('wikipedia.zim', include_content := 'text/html');
SELECT path, content FROM read_zim('wikipedia.zim', include_content := ['text/html', 'text/css']);
```

## Accept-style mimetype matching

The `mimetype` row filter takes a single pattern or a `LIST`, with `image/*` / `*/*`
wildcards:

```sql
SELECT path, mimetype FROM read_zim('wikipedia.zim', mimetype := 'image/*');

-- list order is your preference; rank with SQL
SELECT path FROM read_zim('wikipedia.zim', mimetype := ['image/webp', 'image/png'])
ORDER BY array_position(['image/webp', 'image/png'], mimetype);   -- webp first
```

## Multiple archives, globs, and `FROM 'x.zim'`

`read_zim` accepts one path, a glob, or a `LIST`; rows from each archive are concatenated,
and `include_filepath := true` tells them apart. A bare `.zim` filename in `FROM` is
rewritten to `read_zim` automatically.

```sql
SELECT count(*) FROM read_zim('wikis/*.zim');                          -- glob
SELECT * FROM read_zim(['a.zim', 'b.zim'], include_filepath := true);  -- explicit list
SELECT * FROM 'wikipedia.zim';                                         -- replacement scan
```

Use SQL `LIMIT` / `OFFSET` to bound or paginate a scan — it streams, so `LIMIT` stops the
scan early; there's no separate limit parameter.

## Parallel scans

The plain path-order scan runs **in parallel** across DuckDB's configured `threads` by
default. Entries are partitioned by libzim **cluster order**, so each compressed cluster is
decompressed once even across threads, and the pooled (threadsafe) libzim handle is read
concurrently.

```sql
SET threads = 8;
SELECT count(*) FROM read_zim('wikipedia.zim');                 -- parallel
SELECT * FROM read_zim('wikipedia.zim', parallel := false);     -- single-threaded
```

!!! note "Order is not guaranteed under a parallel scan"
    Like any parallel SQL scan, rows come back in no particular order — add `ORDER BY`, or
    use `parallel := false` for a path-ordered single-threaded scan. Exact lookups, prefix
    listings, and `listing := 'title'` are single-threaded by nature and unaffected.

## Remote archives (S3 / HTTP)

`read_zim` (and every other function here) can open a ZIM on remote storage and
read it by **byte-range requests** — only the bytes a query touches are fetched,
never the whole file. Pass an `s3://`, `http://`, `https://`, `gcs://`, … URL
anywhere a path goes; it rides DuckDB's `httpfs` for transport and auth.

```sql
INSTALL httpfs; LOAD httpfs;   -- once per session

-- open a ~1 GB remote Wikipedia and read one article: fetches ~0.5% of the file
SELECT content
FROM read_zim('https://example.org/wikipedia.zim',
              path := 'A/London', include_content := true, content_as_varchar := true);
```

!!! tip "Use targeted access for big remote archives"
    Range reads make *lookups* and *prefix listings* cheap remotely
    (`path :=`, `path_prefix :=`). A full unfiltered scan still reads every entry's
    dirent; for a huge remote archive prefer `path :=` / `path_prefix :=`, and use
    `parallel := false` for bounded (`LIMIT`) remote listings — the parallel scan
    pre-reads all dirents to partition by cluster, which is wasteful when you only
    want a few rows.

!!! warning "Full-text search is local-only"
    `zim_search` returns no rows on a remote archive: libzim opens the Xapian
    index through a direct file descriptor, which a byte-range reader can't
    provide. Listing, lookup, content, metadata, and `zim_suggest` all work
    remotely; only fulltext search needs a local file.

Remote opens require `enable_external_access` (on by default) and the `httpfs`
extension; a missing `httpfs` surfaces a clear "INSTALL httpfs" error rather than
an opaque open failure.

## Metadata

Metadata is a separate key space from content (two distinct libzim doors).

```sql
SELECT key, value FROM read_zim_metadata('wikipedia.zim');

SELECT zim_metadata('wikipedia.zim', 'Title');
SELECT zim_metadata_keys('wikipedia.zim');     -- LIST(VARCHAR)
SELECT zim_counter('wikipedia.zim');           -- MAP(mimetype -> count) histogram
SELECT zim_info('wikipedia.zim');              -- STRUCT of counts / flags / uuid
```
