# Search & suggestions

## Full-text search

If an archive carries a Xapian full-text index (`zim_info(file).has_fulltext_index`),
`zim_search` queries it:

```sql
SELECT path, title, score, snippet
FROM zim_search('wikipedia.zim', 'compound heterozygous', max_results := 20);

-- paginate with result_offset; both default to (25, 0)
SELECT path FROM zim_search('wikipedia.zim', 'photosynthesis',
                            max_results := 10, result_offset := 10);
```

Returns `(path, title, score DOUBLE, snippet VARCHAR, file VARCHAR)` ranked by relevance.
`score` is the Xapian rank; `snippet` is a best-effort highlighted excerpt (`NULL` when
none is produced). An archive without a full-text index — or the search-less WebAssembly
build — returns no rows rather than erroring.

!!! note "Why `max_results` / `result_offset`?"
    `limit` and `offset` are SQL reserved words and won't parse as bare named parameters
    (the same reason the scan uses `listing`, not `order`). Unlike a relational scan,
    search needs a bounded count up front — libzim's `getResults(offset, count)` — so the
    bound is an explicit parameter rather than a SQL `LIMIT`.

## Title suggestions

`zim_suggest` is title **autocomplete** over the suggestion index — returns
`(path, title, snippet, file)`. Unlike `zim_search`, it works on **every build**: it falls
back to a title-prefix listing where there's no Xapian index, so it's available on
WebAssembly too.

```sql
SELECT path, title FROM zim_suggest('wikipedia.zim', 'Photosyn', max_results := 10);
```

## Federated search

Like `read_zim`, the first argument to both functions can be a single path, a **glob**, or
a `LIST(VARCHAR)`, so a query runs across many archives at once. `max_results` applies
**per archive**, the `file` column names the source of each hit, and you rank/trim across
them in SQL — Xapian scores are per-archive, so not directly comparable:

```sql
SELECT file, path, title, score
FROM zim_search('library/*.zim', 'insulin', max_results := 5)
ORDER BY score DESC LIMIT 20;

SELECT file, title FROM zim_suggest('library/*.zim', 'Photosyn');
```

A no-index archive in the set simply contributes nothing — no error.
