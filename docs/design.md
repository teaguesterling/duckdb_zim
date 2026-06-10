# Design notes

## Three access surfaces, one engine

A ZIM is a read-only, content-addressed store: **path → entry** and **title → entry**,
where each entry is a redirect or a typed blob, plus a small metadata bag and an optional
search index. The extension exposes it through:

1. **Scan / lookup / list** — `read_zim` and the `zim_*` scalars.
2. **Filesystem** — the [`zim://`](filesystem.md) seam, so the rest of the ecosystem reads
   ZIM contents with no coupling.
3. **Search** — `zim_search` / `zim_suggest`.

A process-wide **archive pool** keeps each opened `zim::Archive` alive across queries, so
libzim's decompressed-cluster cache stays warm; every function and the filesystem share one
open handle per file. All libzim contact is isolated in a small access layer
(`src/zim_access.*`); the rest is a plain DuckDB binding over DuckDB-agnostic structs.

## Scraper-agnostic by design

The extension ships **zero** HTML knowledge and **zero** scraper knowledge. Content paths
are opaque strings, so mwoffliner (`A/Foo`) and zimit (`www.example.com/page/`) archives
both "just work". Scraper-specific patterns — reconstructing URLs, stripping article
chrome, detecting provenance via `zim_metadata(file, 'Scraper')` — are documented
[recipes](recipes.md), not hardcoded functions, because those conventions shift across
scraper versions. This keeps the **GPL** surface confined to "read the container"; all rich
processing stays in the MIT ecosystem (`webbed`, `markdown`, `duck_block_utils`) and never
links against the GPL binary.

## License

libzim is **GPL-2.0-or-later**; statically linking it makes the distributed extension a
derivative work, so this extension is GPL too. That is intentional and separate from the
MIT-licensed extension family.

## Why there is no `ATTACH`

`ATTACH 'x.zim' AS wiki (TYPE zim)` was the last planned phase and was **dropped**. A ZIM
is a *dataset* — one logical relation plus metadata plus a search index — not a multi-table
*database*. DuckDB's idioms split cleanly:

- `read_*` functions for a single logical dataset (parquet, csv, json) — which is the ZIM
  shape, and what this extension provides.
- `ATTACH … (TYPE …)` for things that genuinely are multi-table databases (sqlite,
  postgres).

The only structurally table-shaped thing in a ZIM is its **namespaces** (`C` content, `M`
metadata, `W` well-known, `X` index internals). But the two useful ones are already
functions (`read_zim` = `C`, `read_zim_metadata` = `M`), and the rest are low-value: `W` is
essentially the main entry (already `zim_main_entry`), and `X` is opaque Xapian/title index
blobs that `zim_search` already consumes. `ATTACH`'s one non-cosmetic benefit — a warm
handle — is already provided by the archive pool, and aliasing is a one-line `CREATE VIEW`.

If peeking at `W` / `X` is ever needed, an opt-in raw-enumeration flag on `read_zim` is the
honest tool, not a catalog. The catalog shape that *would* earn its keep is the other
direction — a **multi-archive library** to browse and join across a shelf of ZIMs — which
the federated `zim_search` / `zim_suggest` and `read_zim('*.zim')` already partly cover.

See also the verified [libzim semantics](libzim-semantics.md).
