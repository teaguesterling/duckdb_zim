# Remote full-text search design (v0.5)

Status: design / not yet implemented. Captures the profiling that motivates the
approach so v0.5 doesn't re-derive it.

## Problem

Remote (s3/http) archive reading works (v0.3/v0.4): content and metadata stream
via byte-range reads, touching ~0.6% of a multi-GB file. **Full-text search does
not work remotely.** libzim opens the Xapian index with a real local file
descriptor (`getDbFromAccessInfo` → `openFile(filename)` + `Xapian::Database(fd)`),
so over a remote archive `zim_search` returns nothing. The title-listing fallback
(`zim_suggest`) still works.

The obvious fix — download/extract the whole Xapian index to a temp file, then
search it locally — is a blunt instrument. The index is large (see below), so it
re-imports the cost we just eliminated. This doc shows, with measurements, that a
**random-access remote reader for Xapian** is both worthwhile and feasible, and is
the design we should build.

## Evidence (measured)

Profiled against `wikipedia_en_medicine_maxi_2026-04.zim` (2.22 GB, 362,501
articles), a real Wikipedia archive that carries a fulltext index. Tools: libzim
(python binding) + `quest`/`xapian-delve` (Xapian 1.4.22) under `strace -y` on the
extracted single-file glass DB. Methods are reproducible from the scripts noted at
the end.

### Index sizes

The Xapian indexes are entries in a reserved namespace, addressable by id but NOT
by `getEntryByPath("X/...")` — that is why the SQL surface can't see them today.

| index | size | % of archive |
|---|---|---|
| `fulltext/xapian` | 166 MB | 7.5% |
| `title/xapian` | 62 MB | 2.8% |

- The fulltext index holds 102,063 documents; **`has positional information = false`**
  (built without positions → no position table; phrase queries degrade to term-AND
  and cannot blow up the working set).
- Extrapolated to full English Wikipedia (~6.5M articles, ~linear in article count):
  fulltext index ≈ **2.5–3 GB** in the ~100 GB `_maxi` archive (same ~7–8% ratio).
- **Many archives have no index at all.** `wikipedia_en_simple_all_nopic` (982 MB)
  and `..._maxi` (3.47 GB) report `has_fulltext_index = false` AND
  `has_title_index = false` — they ship with zero Xapian; suggest rides the ZIM's
  built-in title-ordered PTR list. Those archives need a different path (below).

### Access pattern — a search touches ~0.5% of the index

Per query, distinct 8 KB glass blocks read (search + retrieve top-25), across query
frequency from 227 to 25,356 matches:

| query | matches | bytes read | % of 166 MB index |
|---|---|---|---|
| pheochromocytoma (rare) | 227 | 0.80 MB | 0.48% |
| insulin | 2,169 | 0.89 MB | 0.53% |
| patient (common) | 25,356 | 0.98 MB | 0.59% |
| diabetes mellitus type | 1,200 | 1.11 MB | 0.67% |
| blood, top-100 | 17,877 | 1.48 MB | 0.89% |

**Key fact: the cost is flat in match count.** "patient" (25k matches) reads about
the same as "pheochromocytoma" (227). Xapian's glass backend does block-skip ranked
retrieval — it does NOT read whole posting lists — so common terms don't explode.

### The access is random, not contiguous

All reads are fixed **8 KB** blocks. ~80–90% are isolated single blocks; median gap
between contiguous runs ~100 KB, max jumps span the whole index; 62–66% of jumps
exceed 64 KB. Reads cluster spatially into **2 table bands** (postlist + docdata),
random *within* each band. There is a little locality (the odd ~280 KB contiguous
posting-list run) but not much. ⇒ The operation is **latency-bound, not
bandwidth-bound**: ~60–180 scattered blocks, not a sweep you can satisfy with a few
big ranges.

### N blocks need not be N round trips (M ≪ N)

Two independent levers:

1. **Cross-query reuse.** Cold first query ≈ 60 blocks (~0.49 MB). Warm marginal
   cost then drops to **~16–27 new blocks (~150 KB)** per query. An entire 8-query
   session's footprint is **1.42 MB**. The reuse is generic byte-range reuse.
2. **Within-query batching.** The ~60 blocks are ~25 *independent* lookups (result
   docids) over a depth-2–3 tree. Descend level-synchronously (every offset at a
   level is known → one parallel fan-out / multipart-range request). Cold critical
   path ≈ **~5 sequential round trips**, bounded by tree *depth*, not block count.

## Design decision

Give Xapian a remote random-access block reader; keep the DB warm; let existing
caches handle reuse. **Do not extract the whole index above a configurable size
threshold** — small indexes take a one-shot local-copy fast path (see below); for
large indexes, build only the reader (caching is generic byte-range caching that
existing layers already do).

### Small-index fast path (local copy below a threshold)

> **Status: shipped.** Both paths below are implemented. The local-copy path uses
> libzim's `OpenConfig::maxLocalSearchIndexBytes`; the range-reader ("Phase 1"
> below) landed as the over-threshold path. The setting is
> `zim_remote_search_max_local_index`.

The "don't extract" rule is for *large* indexes (Medicine 166 MB, full English
~2.5–3 GB). For a *small* index the calculus flips: one sequential range fetch of
the whole blob is simpler and lower-latency than scattered 8 KB block reads over many
round trips. So the reader gates on index size with a configurable threshold:

- Setting: `zim_remote_search_max_local_index` (bytes; default **8 MB**;
  `0` = always range-read; very large = always local-copy).
- If `fulltext/xapian` size ≤ threshold: fetch the whole index blob in one contiguous
  range read **through the same reader** (the index, *not* the archive), spill to a temp
  file, and open Xapian on that fd via the existing `getDbFromAccessInfo` path. One
  request; no glass-reader needed for this case; the temp DB is cached per-archive.
- Else: the Phase-1 random-access glass reader below.

Both honor "no whole-file download" — they fetch at most the index, never the archive.
The threshold is the one knob trading a simpler one-shot fetch (small indexes) against
bounded-bytes random access (large indexes). (The local-copy path shipped first, as the
smaller change; the glass reader followed for big indexes.)

### Phase 1 — Xapian random-access glass reader (the real work)

- Patch Xapian's glass backend to read 8 KB blocks through a pluggable
  `RandomAccessReader` (`getSize`/`readAt`) — the same interface we added to libzim.
  Back it with a DuckDB `FileHandle` (httpfs byte-range) at `index_offset + block*8192`,
  where `index_offset` comes from libzim's `getDirectAccessInformation` (already wired).
- Removes the "Xapian needs a local fd" blocker.
- Also expose the index entries to the SQL surface (they exist as sized items reached
  by id, just filtered out of the content-only `getEntryByPath`).

### Phase 2 — keep it warm & cached (mostly free)

- Pin the open Xapian `Database` in the same per-DB warm pool as the archive
  (the per-DatabaseInstance ObjectCache pool, `src/zim_archive_pool.hpp`) → in-session
  interior reuse at exact 8 KB granularity via Xapian's own block cache. This alone
  delivers lever #1 with no new caching code.
- Read blocks through the httpfs `FileHandle` so DuckDB's `ExternalFileCache` /
  the `cache_httpfs` extension sit underneath transparently, adding cross-session
  persistence.
- **DECIDING CAVEAT — granularity.** Reads are scattered 8 KB. If a cache/fetch layer
  works in coarse blocks (256 KB–1 MB), 60 scattered reads amplify to tens of MB cold
  and the win evaporates. Tying into `cache_httpfs` is correct **iff its block size is
  tunable to ~8–64 KB**. Verify this. If not, add a thin 8 KB-aligned LRU in the
  reader; the warm Xapian Database covers in-session reuse regardless.

### Phase 3 — latency polish (only if measurements demand it)

- Measure cold-search wall-clock first; the warm path is likely already interactive.
- If cold latency hurts: level-synchronous batched descents + parallel/multipart-range
  fan-out for the independent docdata fetches (~5 RTT critical path).

### Out of scope: no-index archives

Simple/nopic archives have no Xapian at all — this design does not apply. Their path
is DuckDB-native FTS over pulled content (or accept suggest-only). Decide per archive
via `has_fulltext_index`.

## Open questions to verify during implementation

- `cache_httpfs` / `ExternalFileCache` effective block granularity (the deciding caveat).
- Whether patching upstream Xapian glass is acceptable vs. vendoring (mirrors the
  libzim overlay-port decision).
- Cold vs warm search wall-clock over real S3/HTTP at representative RTT.
- Full-English-scale validation (deeper B-tree → a few more blocks/descent, still
  logarithmic; ratio should only improve as the index grows).

## Reproducing the profiling

Index lives at reserved path `fulltext/xapian`; extract via python-libzim
`_get_entry_by_id` (the public `getEntryByPath` filters it out). The glass DB opens
with stock Xapian 1.4.x. Queries match with `quest --stemmer=none` (the ZIM index
stores unstemmed terms). Access pattern captured with
`strace -y -e trace=pread64` (glass uses `pread`, not mmap, so the trace is complete).
