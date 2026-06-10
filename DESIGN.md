# duckdb_zim — Design

A DuckDB extension to read `.zim` files (Kiwix / openZIM archives) via libzim.

Status: original design rationale. Phase-1 core is implemented in `src/`; conventions
follow the `markdown` / `yaml` / `webbed` family (`read_zim*` table fns, `zim_*` scalars,
named `:=` params).

> **SUPERSEDED IN PART — read `docs/libzim-semantics.md` first.** Empirical verification
> against a real archive (via python-libzim) overturned the namespace assumptions in §3
> and §4.1 below. The authoritative, verified model lives in `docs/libzim-semantics.md`.
> Corrections, in brief:
> 1. **Namespace-led `zim://` grammar (§3.1–3.2) is dropped** → content-path-first:
>    `zim://wikipedia.zim/A/Photosynthesis`. libzim 7's high-level API does **not**
>    resolve `M/`/`W/`/`X/` paths; metadata is a separate door (`getMetadata`).
> 2. **The `namespace` column (§4.1) is dropped.** `A/`, `I/` are path text, not
>    namespaces; a content scan is the C namespace, so the column would be a constant
>    fiction. The implemented `read_zim` schema is in `README.md` / `src/read_zim.cpp`.
> 3. The two-door model (content via path; metadata via key) is reflected in the code:
>    `read_zim` = content, `read_zim_metadata` / `zim_metadata` = metadata.
>
> Everything else here (the three-surface analysis, ATTACH weighing, ArchivePool,
> webbed integration, build order, mimetypes/Counter) stands as written.
>
> 4. **Surface C / phase 4 (`ATTACH … TYPE zim`) was evaluated and DROPPED** for v0.2.0.
>    A ZIM is a dataset, not a multi-table database, so the `read_*` idiom fits and the
>    catalog idiom does not; the warm-handle benefit is already the ArchivePool's. The §2
>    weighing below leans this way already; the final rationale lives in
>    `docs/design.md`. v0.2.0 is feature-complete.

---

## 0. Decisions made / pending

### 0.1 Licensing — GPL (decided)

libzim is **GPL-2.0-or-later**. Statically linking it makes the distributed
`.duckdb_extension` a derivative work that must ship GPL. **Accepted.** This one
extension is GPL-2.0-or-later, separate from the MIT family. It has publishing
implications (community-extensions accepts GPL; no code reuse back into the MIT
extensions) but is not a blocker.

Design consequence that follows from this: **keep HTML/XML/markdown parsing OUT of
the zim binary.** Let `webbed` (MIT) own all of that via the filesystem seam (§5). The
GPL surface stays confined to "read the ZIM container"; all the rich processing lives
in the MIT ecosystem and never links against the GPL binary.

### 0.2 WASM — a spike gated by xapian, not a flat "no" (revised)

Earlier call ("native-only") was too pessimistic. The dependency stack *individually*
compiles to WASM: DuckDB-WASM already ships **zstd** and the **ICU** extension, which
is existence proof for the two hard libs; **liblzma** is portable C. The real blocker
narrows to **xapian** (full-text search). So:

- Make `xapian`/search a **compile-time optional feature**.
- A core-only WASM build (read + metadata + listing + `zim://` filesystem, no FTS)
  is a **spike worth doing**, not an obvious no.

Two things that actually decide the spike (verify, don't assume):

1. Does **libzim-without-xapian still pull in ICU**? It may use ICU for title/suggestion
   normalization independent of FTS. If dropping xapian also drops ICU, the WASM stack
   collapses to zstd + lzma (both trivial) and WASM becomes likely.
2. Has anyone built **libzim itself** under emscripten? The deps compiling is necessary
   but not sufficient — libzim's mmap-based archive access + meson build are the unproven
   part. (kiwix-js is a *pure-JS* reader, NOT libzim-via-WASM, so it is not a proof point.)

### 0.3 Dependency reuse — mostly not worth it (note)

"These libs are already in DuckDB, reuse them" splits in two:
- **Feasibility reuse** (they compile to WASM): real, see §0.2.
- **Binary/symbol reuse** (link libzim against DuckDB's copies): mostly no. DuckDB
  *deliberately* namespaces its vendored third-party symbols (its zstd is `duckdb_zstd`,
  not `ZSTD_*`) so loaded extensions don't collide. An extension is a separate shared
  object that brings its own static deps. Realistic outcome: libzim statically links its
  own zstd/lzma/icu; the duplicate in DuckDB is tolerated (hidden, no clash). Cost is
  binary size, not correctness. Only ICU *data* in WASM (tens of MB) would justify the
  effort, and it sits behind the extension boundary, so it's hard.

---

## 1. What a ZIM actually is (drives the whole abstraction)

A ZIM is **not** relational. Stripped down:

- A read-only content store: **path → entry** and **title → entry**.
- Each entry is a **redirect** or an **item** (item = mimetype + size + blob).
- **Namespaces** in the on-disk path: `C` content, `M` metadata, `W` well-known,
  `X` index internals. libzim 7 *hides* these in its high-level API (content via
  `getEntryByPath`, metadata via `getMetadata`) but they remain the format's true
  structure — which is why the filesystem grammar (§3) leads with them.
- A **main entry** (landing page).
- Optional **Xapian full-text index** and **title/suggestion index**.
- A **`Counter`** metadata value: a self-describing mimetype→count histogram.
- Archive-level scalars: UUID, entry/article/media counts, checksum, filesize.

Honest type: *a read-only, namespaced filesystem of paths to typed blobs, plus a search
index, plus a small bag of metadata.* One logical relation (entries), not many tables.
That single fact is why §2 leads with table functions + a filesystem, not ATTACH.

libzim C++ surface (v9.x) the design maps onto:

```cpp
zim::Archive a("wikipedia.zim");
a.getEntryByPath("A/Foo");           // exact; throws EntryNotFound
a.getEntryByTitle("Foo");
a.findByPath("A/");                  // PREFIX → EntryRange (iterable)   ← listing/prefix
a.findByTitle("Calc");               // PREFIX over title index          ← listing/prefix
for (auto e : a.iterByPath())  {...} // full listing, path order
for (auto e : a.iterByTitle()) {...} // full listing, title order
a.getMetadata("Title");  a.getMetadataKeys();          // M namespace
a.getMainEntry(); a.getUuid();
a.getEntryCount(); a.getArticleCount(); a.getMediaCount();
a.hasFulltextIndex(); a.hasTitleIndex(); a.hasChecksum(); a.getFilesize();

auto e = a.getEntryByPath("A/Foo");
if (e.isRedirect()) e.getRedirectEntry();
else { auto it = e.getItem(true); it.getMimetype(); it.getSize(); it.getData(); }

zim::Searcher s(a); zim::Query q; q.setQuery("photosynthesis");
auto res = s.search(q).getResults(0, 20);   // iterable → entries (+snippet, recent libzim)
```

Performance fact that anchors §6: `zim::Archive` holds a **cluster cache** — decompressed
clusters stay warm for the handle's lifetime. Reopening per query throws that away.

---

## 2. Three access surfaces (the real "options" answer)

Not read-vs-attach. Three orthogonal surfaces, sharing one engine:

| Surface | Shape | Good at | Honesty |
|---|---|---|---|
| **A. Scan / lookup / list** | `read_zim(...)` + `zim_*` scalars | inventory, filtering, prefix listing, point lookup, FTS, metadata | high |
| **B. Filesystem** | `zim://archive.zim/C/A/Foo` | piping ZIM contents into *other* extensions | high — a ZIM literally is a namespaced FS |
| **C. Catalog** | `ATTACH 'x.zim' AS wiki (TYPE zim)` | ergonomics, `SHOW TABLES`, warm handle | medium — implies "database" over a frozen blob |

Build A → B → (optional) C.

**Weighing ATTACH (since it was the original question):** its only non-cosmetic benefit
is a warm handle / warm cluster cache across queries. That is **not coupled to ATTACH** —
it's a property of "keep the handle open," delivered by a global `ArchivePool` (§6) that
the table function, the filesystem, and a future ATTACH all share. So build the honest
scan/lookup surface first; warm-cache comes free; add ATTACH last as pure sugar, read-only,
writes throwing. There's a type-honesty angle too: the table function's contract ("a
function yielding entry rows from this file") matches reality; ATTACH's ("a catalog of
tables") overpromises against a read-only blob. One nudge toward ATTACH: it gives the
filesystem a clean alias namespace (§3), so the two can share aliases.

---

## 3. The `zim://` filesystem grammar

### 3.1 Grammar — namespace-led, REST-style, plain slashes

```
zim://<archive>.zim/<NS>/<path>

zim://wikipedia.zim/C/A/Photosynthesis     -- content article
zim://wikipedia.zim/M/Title                -- metadata value
zim://wikipedia.zim/W/mainPage             -- well-known
```

Decisions:

- **No `!` boundary.** The DuckDB-native precedent (tarfs `tar://…/ab.tar/a.csv`,
  zipfs `zip://…/x.zip/file.shp`) uses **plain slash concatenation** with
  **extension-based boundary detection**. The `!` convention is Java/Hadoop/Commons-VFS
  (`jar:file:…!/entry`); fsspec uses `::`. Matching the DuckDB neighbors also happens to
  read more REST-y, so the ecosystem fit and the REST goal agree.
- **Namespace-led** (the segment after the archive is the ZIM namespace letter). Gives
  the filesystem one uniform path space — content, metadata, and well-known all
  addressable as paths — and maps 1:1 to the entries table's `(namespace, path)`.
- **Boundary detection**: longest prefix ending in `.zim` — *and* `.zim[a-z][a-z]`,
  case-insensitive, because large archives split into `wikipedia.zimaa`, `…zimab`, ….

### 3.2 Namespace router (reconciles "lead with namespace" vs "close to the C++ API")

These two goals are in genuine tension — libzim7 hides namespaces. Resolution: the URL
is namespace-led, and the **namespace segment routes to the closest libzim7 accessor**:

```
/C/<path>  → getEntryByPath(<path>)     (content)
/M/<name>  → getMetadata(<name>)        (metadata)
/W/<path>  → well-known entries
/X/...     → index internals — probably do not expose
```

URL stays honest to the format; each branch stays honest to the API.

### 3.3 Composition payoff

Once registered, the whole ecosystem reads *into* ZIM contents with zero coupling:

```sql
SELECT * FROM read_html('zim://wikipedia.zim/C/A/Photosynthesis');   -- webbed (HTML)
SELECT * FROM read_xml ('zim://wikipedia.zim/C/...diagram.svg');     -- SVG is XML!
SELECT * FROM read_blob('zim://wikipedia.zim/C/I/some_image.webp');  -- binary out
```

### 3.4 Open implementation points

- DuckDB's `FileSystem` expects seekable byte ranges; a ZIM item is a decompressed blob.
  Simplest correct impl materializes the item and serves reads from memory (fine for
  articles; watch memory on large media / on webbed's HTML DOM which is full-DOM, not SAX).
- `Glob()` is the path-pattern **listing** mechanism (§4.2): enumerate `iterByPath()` /
  `findByPath(prefix)`, mimetype-filterable, e.g. `zim://x.zim/C/A/*`.
- Alias form `zim://wiki/C/A/Foo` once ATTACH exists — resolve alias first, then fall
  back to filesystem path (define precedence to avoid alias/path ambiguity).

---

## 4. API surface

### 4.1 `read_zim(files, [params...])` — one row per entry

```sql
SELECT * FROM read_zim('wikipedia.zim');
SELECT * FROM read_zim('*.zim');               -- glob, like read_markdown
SELECT * FROM 'wikipedia.zim';                 -- replacement scan
```

Columns:

| column | type | notes |
|---|---|---|
| `namespace` | VARCHAR | `C`/`M`/`W`/`X`; default scan = `['C']` |
| `path` | VARCHAR | entry path within namespace (verify exact getPath() semantics) |
| `title` | VARCHAR | |
| `mimetype` | VARCHAR | NULL for redirects |
| `is_redirect` | BOOLEAN | |
| `redirect_path` | VARCHAR | NULL unless redirect |
| `size` | UBIGINT | item size; NULL for redirects |
| `content` | BLOB | **lazy** — fetched only if projected or `include_content := true` |
| `file_path` | VARCHAR | only with `include_filepath := true` (alias `filename`) |

Pushdown (what makes it fast):

- *Projection*: never call `item.getData()` unless `content` is referenced → listing
  millions of entries stays in milliseconds instead of decompressing the archive.
- *Filter on `path`/`title`*: `=` → `getEntryByPath`/`getEntryByTitle`; prefix `LIKE 'A/%'`
  → `findByPath` (see §4.2). Turns a key lookup into O(1)-ish, not a scan.
- *Filter on `namespace`/`mimetype`*: skip non-matching during iteration.

### 4.2 Listing & prefix filtering — FIRST-CLASS

Listing and prefix filtering are core to a content store, not an afterthought, and they
map directly to libzim's prefix/iteration API rather than to a full scan + WHERE.

Params on `read_zim`:

- `path_prefix := NULL` → backed by `findByPath(prefix)` (efficient prefix range, no
  full scan).
- `title_prefix := NULL` → backed by `findByTitle(prefix)` (title-sorted; good for
  autocomplete-style "all titles starting with 'Calc'").
- `listing := 'path' | 'title'` → `iterByPath` vs `iterByTitle`. NOTE (verified against
  C++ libzim 9.7.0, correcting the earlier "either ordering is free" assumption): the
  two cover DIFFERENT sets, so this picks a *listing*, not just an order. `iterByPath`
  yields all user entries; `iterByTitle` yields only FRONT_ARTICLE entries (the title
  listing), so `listing := 'title'` returns the articles, not every entry. `title_prefix`
  (`findByTitle`) rides the same title listing. For all entries in title order, use the
  default path listing and `ORDER BY title` in SQL. (Named `listing`, not `sort`/`order`:
  it both filters and orders, and `ORDER` is a SQL reserved word that won't parse as an
  unquoted named parameter anyway.)
- `namespaces := ['C']`, `mimetype := NULL` as in §4.1.

```sql
-- cheap listing: names only, no content fetched (projection pushdown)
SELECT path, title, mimetype
FROM read_zim('wikipedia.zim', path_prefix := 'A/Cal');

-- all CSS assets in the archive
SELECT path, size FROM read_zim('wiki.zim', mimetype := 'text/css');
```

Two listing mechanisms, one engine: **relational** (`read_zim` + prefix/filters) and
**path-pattern** (filesystem `Glob`, e.g. `read_blob('zim://wiki.zim/C/A/*')`). Both back
onto `findByPath`/`iterByPath`. A thin `zim_list(file, prefix, by := 'path')` convenience
wrapper is possible but probably redundant given the params — decide later.

### 4.3 Metadata — CRUCIAL, and there are two distinct kinds

Conflating these is a trap. Both matter:

**(a) M-namespace content metadata** (openZIM spec keys), via `getMetadata`:

```sql
-- direct single-key lookup (the crucial pattern)
SELECT zim_metadata('wikipedia.zim', 'Title');
SELECT zim_metadata('wikipedia.zim', 'Language');     -- ISO 639-3, may be comma-list
SELECT zim_metadata('wikipedia.zim', 'Date');         -- ISO yyyy-mm-dd

-- list available keys
SELECT zim_metadata_keys('wikipedia.zim');

-- full table
SELECT key, value, raw FROM read_zim_metadata('wikipedia.zim');
```

`read_zim_metadata(files)` → `(key VARCHAR, value VARCHAR, raw BLOB[, file_path])`.
`raw` carries binary values like the illustration. Standard keys: `Name`, `Title`,
`Creator`, `Publisher`, `Date`, `Description`, `LongDescription`, `Language`,
`Illustration_48x48@1` (PNG), and optional `Tags`, `Flavour`, `Source`, `Counter`,
`Scraper`, `License`, `Relation`.

`Counter` is special — a self-describing mimetype histogram
(`text/html=11604;image/webp=987;text/css=3;…`). Surface a parsed helper:

```sql
SELECT zim_counter('wikipedia.zim');   -- MAP(VARCHAR mimetype, BIGINT count)
```

This makes "what's in this archive" a query, not an assumption.

**(b) Archive-level structural info**, via `Archive` methods (NOT the M namespace):

```sql
SELECT zim_info('wikipedia.zim');
-- STRUCT(uuid, entry_count, article_count, media_count, main_entry_path,
--        has_fulltext_index, has_title_index, has_checksum, filesize, is_multipart)
```

### 4.4 `zim_search(files, query, [limit, offset])` — Xapian FTS (optional feature)

```sql
SELECT path, title, score, snippet
FROM zim_search('wikipedia.zim', 'compound heterozygous', limit := 20);
```

`(path, title, score DOUBLE, snippet VARCHAR)`; snippet if the libzim/ZIM version
supports it, else NULL. Errors cleanly (or falls back to `findByTitle`) when
`has_fulltext_index` is false. Search is inherently a *function with a query*, not a
table — stays a table function regardless of ATTACH. `zim_suggest(file, prefix, …)`
sibling maps to the title/suggestion index. Compile-time gated (§0.2).

### 4.5 Scalar helpers (mirror the `md_*` family)

```sql
zim_get_content(file, path[, resolve])   -- BLOB single-entry fetch
zim_get_text(file, path)                 -- VARCHAR; mimetype-aware (text/* only)
zim_has_entry(file, path)                -- BOOLEAN
zim_redirect_target(file, path)          -- VARCHAR
zim_main_entry(file)                     -- VARCHAR landing path
zim_mimetype(file, path)                 -- VARCHAR
zim_metadata(file, key) / zim_metadata_keys(file) / zim_counter(file) / zim_info(file)
```

### 4.6 Mimetypes (the archive tells you — see `Counter`)

Don't guess the distribution; query `zim_counter`. Common set:

- **Text/markup**: `text/html` (the bulk by count), `text/css`, `application/javascript`,
  occasional `text/plain`.
- **Images**: `image/webp` (modern mwoffliner is webp-heavy), `image/png`, `image/jpeg`,
  `image/svg+xml` (← **XML**, so it flows into webbed's XML side), `image/gif`.
- **Fonts**: `font/woff2`, `application/font-woff`, ttf/eot.
- **Media** (some collections): `video/webm`, `audio/ogg`, `audio/mpeg`.
- **Documents** (collection-specific): `application/pdf`, `application/epub+zip`
  (Gutenberg often bundles epub alongside HTML).
- **Fallback/internal**: `application/octet-stream`, X-namespace Xapian indexes.

Implications: `content` stays **BLOB** (most entries are binary by size); `zim_get_text`
/ `content_as_varchar` apply only to text mimetypes and should NULL-or-error on binary;
webp matters if anyone wants images out of a modern wiki ZIM (downstream's problem, but
note it in the README).

### 4.7 No custom `ZIM` type (intentional deviation)

The `MARKDOWN` type earns its keep because markdown flows through many functions. ZIM
content is heterogeneous (HTML, webp, woff2, …); a single type adds no dispatch value and
would misrepresent the payload. Use `BLOB`/`VARCHAR` and let `webbed`/`markdown` own typed
parsing via the `zim://` filesystem.

### 4.8 Optional later: `ATTACH ... TYPE zim`

```sql
ATTACH 'wikipedia.zim' AS wiki (TYPE zim);     -- READ_ONLY enforced; writes throw
SELECT * FROM wiki.entries WHERE mimetype = 'text/html';
SELECT * FROM wiki.metadata;
SELECT * FROM zim_search('wiki', 'photosynthesis');
```

Thin layer over the `ArchivePool`: `wiki.entries` = `read_zim` bind, `wiki.metadata` =
`read_zim_metadata` bind. Shares its alias with the filesystem (§3.4). Built last, only
if the catalog ergonomics earn the StorageExtension surface area.

---

## 5. Playing nice with `webbed` (and the rest of the family)

Principle: **zim exposes no HTML/XML knowledge.** Two zero-coupling seams; everything
else is documented recipes. This keeps the GPL boundary clean (§0.1) and follows
coordination-not-control — narrow interface, independent release cycles.

ZIM articles are **HTML** (mwoffliner/zimwriterfs/zimit all emit `text/html`); there is no
markdown/rst convention. So the primary consumer is **webbed**, not markdown.

### 5.1 Seam 1 — filesystem (paths)

webbed's `read_html` already reads through DuckDB's FileSystem layer (`read_html('https://…')`
works today), so `zim://` routes for free:

```sql
SELECT * FROM read_html('zim://wikipedia.zim/C/A/Photosynthesis');
SELECT * FROM read_html('zim://wikipedia.zim/C/A/*');     -- via zim Glob()
```

### 5.2 Seam 2 — value (HTML type)

`read_zim` hands content as BLOB/VARCHAR; webbed's scalars auto-cast and process:

```sql
SELECT path,
       html_extract_text(content, '//h1')[1] AS heading,
       html_extract_links(content)           AS links
FROM read_zim('wikipedia.zim', include_content := true)
WHERE mimetype = 'text/html';
```

webbed API in play: `read_html(files, record_element := …)`, `html_extract_text(html, xpath)`
(→ LIST, XPath 1.0), `html_extract_tables(html)`, `html_extract_links/images(html)`,
`html_to_duck_blocks(html)`, the `HTML` type.

### 5.3 The caveat that actually bites: mwoffliner chrome

Wikipedia article HTML is wrapped in nav/infobox/footer/edit-link chrome. Naive
`//body` text extraction returns garbage-surrounded text. The article body lives in a
known container (`mw-parser-output` / `#mw-content-text`) — but the exact selector shifts
across mwoffliner versions and Vector skins. **zim must not hardcode this** (content- and
version-specific; webbed's job). Deliverable = a documented recipe with the common
content XPaths, flagged version-dependent.

### 5.4 High-value recipes (design toward these — Lifeboat)

**Build your own FTS for ZIMs lacking a Xapian index:**

```sql
CREATE TABLE corpus AS
SELECT path, title,
       html_extract_text(content, '//div[contains(@class,"mw-parser-output")]')[1] AS body
FROM read_zim('wikimed.zim', include_content := true)
WHERE mimetype = 'text/html';
PRAGMA create_fts_index('corpus', 'path', 'title', 'body');
```

**Corpus-wide link graph:** `html_extract_links(content)` → normalize relative hrefs
(mwoffliner internal links are entry paths) → join back to the entries table → edge list
→ graph algorithms (recursive CTEs / your relationship tooling).

**HTML → duck_blocks → markdown → local LLM:** `html_to_duck_blocks(content)` →
`db_blocks_to_text` → clean markdown, the right shape to feed Lifeboat's offline 3B/7B
models as retrieved context. ZIM = corpus, webbed + duck_block_utils = cleaner, DuckDB =
retrieval.

### 5.5 Coupling policy

**Zero webbed references in the zim binary.** Ship integration as a recipes doc plus,
optionally, a separate `.sql` macro pack assuming zim + webbed + duck_block_utils loaded.
Macros don't link, so no license issue — it's purely about independent release cycles and
minimal GPL surface.

---

## 6. Engine: `ArchivePool` (shared by all surfaces)

Decouples "open & cached" from "how the user addresses it" — the move that makes the
ATTACH-for-warm-cache argument moot.

```cpp
class ArchivePool {
  // canonical path -> shared_ptr<zim::Archive>, refcounted; the archive's warm
  // cluster cache lives as long as anyone holds it.
  shared_ptr<zim::Archive> Get(const string &path);
  void SetClusterCacheSize(...);   // PRAGMA zim_cache_size
};
```

- `read_zim` → `Get(path)` → warm cache across queries **without** ATTACH.
- A future ATTACH holds a `shared_ptr` for the attachment lifetime → same warmth.
- The `zim://` filesystem uses the same pool.
- libzim reads are thread-safe → DuckDB parallel scan partitions path/index ranges
  across threads.

---

## 7. Repo layout (cloned from markdown conventions)

```
duckdb_zim/
├── .github/workflows/        # CI: linux/macos/windows × {xapian on, off}; WASM spike job
├── docs/                     # readthedocs/mkdocs
├── duckdb/  extension-ci-tools/   # submodules
├── scripts/
├── src/
│   ├── zim_extension.cpp     # Load(): register fns, scalars, filesystem, (attach)
│   ├── archive_pool.cpp/.hpp
│   ├── read_zim.cpp          # table fn, bind/init/scan, proj+filter pushdown, prefix/list
│   ├── read_zim_metadata.cpp
│   ├── zim_scalars.cpp       # zim_metadata / zim_counter / zim_info / zim_get_* / ...
│   ├── zim_search.cpp        # #ifdef ZIM_WITH_XAPIAN
│   ├── zim_filesystem.cpp    # zim:// FileSystem + Glob() (namespace-led grammar)
│   └── zim_storage.cpp       # ATTACH TYPE zim (phase 4, optional)
├── test/sql/ test/data/      # tiny handmade .zim fixtures via libzim creator
├── CMakeLists.txt  extension_config.cmake  Makefile
├── vcpkg.json                # { "dependencies": [ {"name":"libzim","features":["xapian"]} ] }
├── vcpkg-configuration.json
└── README.md                 # GPL notice + WASM-status + webp note up top
```

libzim is in vcpkg (v9.4.1, Jan 2026). `xapian` a CMake option so a no-search /
WASM-spike build is possible. Test fixtures: generate a 3-article archive (one redirect,
one webp, metadata incl. `Counter`, a fulltext index) with the libzim **creator** API so
tests don't need a multi-GB download.

---

## 8. Build order

1. **`ArchivePool` + `read_zim` (incl. listing/prefix params + pushdown) + metadata
   (`read_zim_metadata`, `zim_metadata`, `zim_metadata_keys`, `zim_counter`, `zim_info`)
   + core `zim_*` scalars.** Replacement scan + glob. Complete, useful, conventions-matching.
2. **`zim://` filesystem + `Glob()`** (namespace-led grammar). Unlocks webbed/markdown/etc.
   over ZIM contents — highest leverage per line, the real reason for Lifeboat.
3. **`zim_search` (xapian)** — optional compile feature.
4. **`ATTACH ... TYPE zim`** — only if catalog ergonomics earn their keep.

WASM spike can slot in after phase 2 (it needs no xapian).

---

## 9. Open questions to verify against a real archive (early)

- Exact `getPath()` / `getEntryByPath` namespace semantics in libzim 7/9 (does the
  content path include/expect `C/`?). Decides the `(namespace, path)` split AND the §3.2
  router for `/C/`, and whether `W`/`X` are reachable via the public API or only raw iter.
- `findByPath` / `findByTitle` prefix semantics w.r.t. namespaces (powers §4.2 listing).
- Snippet availability across libzim versions for `zim_search`.
- Whether libzim-without-xapian still links ICU (decides the WASM spike, §0.2).
- libzim under emscripten at all (mmap + meson) — the actual WASM unknown.
- `zim://` boundary detection across split archives (`.zimaa…`) and alias/path precedence.
- Memory policy for the filesystem on large media (materialize vs. range-read) given
  webbed builds a full HTML DOM (no SAX for HTML).

## 10. Cross-book / catalog layer constraint (from the ZIM librarian)

This concerns a future *multi-book browsing* layer, not the single-archive reader. When
linking across books, the kiwix content route is the on-disk **filename stem including the
date** (`/content/khanacademy_en_all_2023-03`, `/content/osm-united-states-2026-05-05`),
NOT the archive's `Name` metadata (which is stable and dateless) and NOT `Title`. Building
content links from `Name` 404s; use the filename stem, or read the `href` from
`/catalog/v2/entries`. duckdb_zim already exposes both sides of the join — `file_path`
(via `include_filepath := true`) and `zim_metadata(file, 'Name')` — so a browsing layer can
map catalog `Name` -> filename/content path rather than reconstructing the URL. Phase-1
content paths are intra-archive (`A/Photosynthesis`) and unaffected.
