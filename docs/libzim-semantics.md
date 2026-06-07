# libzim semantics — verified findings

Resolved empirically with **python-libzim** (PyPI `libzim`, same C++ core) against a
generated test archive (`test/oracle/test.zim`). These supersede the unverified
assumptions in §3/§4/§9 of `duckdb_zim_DESIGN.md`. Re-verify against the pinned C++
libzim before relying on edge cases, but the model below held cleanly.

## The decisive result: namespaces are not a user-facing addressing dimension

New archives report `has_new_namespace_scheme == true`. Under the new scheme:

- An item stored at path `A/Photosynthesis` reads back with `entry.getPath() == "A/Photosynthesis"`.
  **No `C/` is prepended.** The `A/`, `I/`, … prefixes mwoffliner uses are ordinary
  **path text inside the content namespace**, not libzim namespaces.
- `getEntryByPath` resolves a content path with or without a leading `C/`
  (`"C/A/Photosynthesis"` works and normalizes back to `"A/Photosynthesis"`).
- `getEntryByPath` does **NOT** resolve `M/…`, `W/…`, or `X/…`. Those returned
  `has == false`. Metadata, well-known, and index entries are **not** addressable through
  the high-level path API.

### Implications (design corrections)

1. **Drop the namespace-led `zim://x.zim/<NS>/<path>` grammar.** Use content-path-first:
   `zim://wikipedia.zim/A/Photosynthesis`. Normalize a leading `C/` away on lookup.
2. **Drop the `namespace` column** from the default `read_zim` content scan — it would be
   a constant-`C` fiction and the `A/`/`I/` are not namespaces.
3. **Two doors**, mirrored in the API: content (`getEntryByPath` / `iterByPath`) vs
   metadata (`getMetadata` / `getMetadataKeys`). `read_zim` = content; `read_zim_metadata`
   = metadata. `W`/`X` are internal; only expose via an opt-in raw enumeration if ever.

## Counts and flags (archive-level, distinct from M-namespace metadata)

For the 5-item + 1-redirect test archive:

| accessor | value | meaning |
|---|---|---|
| `getEntryCount()` | 6 | user/content entries (the `read_zim` universe) |
| `getAllEntryCount()` | 16 | content + metadata + well-known + index internals |
| `getArticleCount()` | 3 | front articles (FRONT_ARTICLE hint, here the HTML) |
| `getMediaCount()` | 1 | media items |
| `hasFulltextIndex()` | true | Xapian FTS present |
| `hasTitleIndex()` | true | title/suggestion index present |
| `getUuid()` | 2088a933-… | |
| `getFilesize()` | 60361 | |
| `isMultiPart()` | false | |

`getAllEntryCount()` enumeration is ordered by internal namespace then path: content (C)
first `[0, getEntryCount())`, then metadata (M), then well-known (W: `mainPage`), then
index (X: `fulltext/xapian`, `listing/titleOrdered/v1`, `title/xapian`). The high-level
`getPath()` strips the namespace letter, so namespace is **not** recoverable from
`getPath()` alone — only inferable from these id ranges (fragile; avoid depending on it).

## Lookups

- `getEntryByPath(path)` / `hasEntryByPath(path)` — content; tolerates optional `C/`.
- `getEntryByTitle(title)` / `hasEntryByTitle(title)` — by title; returns content entry.
- `getMainEntry()` — **is itself a redirect** (`mainPage` → `A/Photosynthesis`). Resolve
  the redirect to get the real landing path.
- Prefix listing in C++: `findByPath(prefix)` / `findByTitle(prefix)` return iterable
  EntryRanges (not exposed in the python binding; verify exact prefix scoping in C++).
- Full enumeration in C++: `iterByPath()` / `iterByTitle()` EntryRanges over content.

## Entries, redirects, content

- `entry.isRedirect()`; redirect → `entry.getRedirectEntry().getPath()`.
- item: `getItem(true)` (follow redirects) → `getPath/getTitle/getMimetype/getSize/getData`.
- `getData()` → `zim::Blob` (`.data()`, `.size()`); content is binary → DuckDB **BLOB**.
  Confirmed read of the HTML item returned the exact stored bytes.

## Metadata

- `getMetadataKeys()` → e.g. `[Counter, Creator, Date, Description, Language, Title]`.
- `getMetadata(key)` → raw bytes (string). `getMetadataItem(key)` → Item (has mimetype;
  `Counter` is `text/plain`). Illustration metadata (`Illustration_48x48@1`) is a PNG blob
  via `getIllustrationItem(size)` / `getIllustrationSizes()` / `hasIllustration()`.
- **`Counter` format** confirmed: `image/png=1;text/css=1;text/html=3` — split on `;`
  then `=` → `MAP(VARCHAR mimetype, BIGINT count)`. This is the self-describing mimetype
  histogram; "what's in this archive" is a query, not an assumption.

## Full-text search

- `Searcher(archive).search(Query().setQuery("plants"))`.
- `getEstimatedMatches()` → est. count; `getResults(offset, count)` → results.
- python yielded **paths** only (`['A/Photosynthesis', 'A/Chlorophyll']`). C++
  `SearchResultSet` iterator additionally exposes title/score/snippet on recent libzim —
  verify availability; treat snippet/score as optional (NULL when absent).

## Open items still to confirm against C++ libzim specifically

- Exact `findByPath` / `findByTitle` prefix semantics and whether prefixes are matched on
  the namespace-stripped content path (expected: yes).
- `SearchResultSet` iterator fields (snippet/score) across versions.
- Whether `iterByPath()` yields redirects as well as items (python enumeration did include
  the redirect among content). Assume yes; mark redirects via `isRedirect()`.
