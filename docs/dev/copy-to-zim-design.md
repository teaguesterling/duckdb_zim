# `COPY … TO … (FORMAT zim)` — design

> **Status: design, not built.** Scoped for v1 (items + metadata options); the
> source-archive content mode is v2 and is specified here only far enough to keep the
> v1 column contract from needing a rename.
>
> Empirical results below are measured against **libzim 9.7.0** (our pinned vcpkg
> overlay, port-version 2) unless stated otherwise. Results credited to the ZIM-Librarian
> session were measured against a python-libzim 3.12.0 wheel, which bundles a *different*
> libzim — where the two disagree, 9.7.0 is authoritative for this extension.

## 1. What this is

The extension currently only reads. This adds the write half: turn the result of any
query into a `.zim` archive.

```sql
COPY (SELECT path, title, mimetype, content FROM articles)
TO 'out.zim' (FORMAT zim, TITLE 'My Archive', LANGUAGE 'eng');
```

The design goal is that **`read_zim`'s output schema is `COPY`'s input schema**, so a
ZIM→ZIM copy is close to an identity and can serve as a conformance test. That is a goal,
not a description: §6 covers the reader and writer changes needed to make it true, and §8
covers where the identity still does not hold once they are made.

## 2. The engine constraint that shapes everything

libzim's `Creator` is a **pull** interface. `Item::getContentProvider()` is called by
libzim on its own worker threads, whenever it is ready — *not* when you call `addItem()`.
Measured: adding 8 items produced **0** content reads during the add loop and 16 reads
afterwards (2 per item — one for the data, one returning an empty `Blob` for EOF).

DuckDB's `copy_to_sink` is a **push** interface, and the `DataChunk` it hands you has its
vectors recycled the moment you return.

These do not compose for free. Anything the sink receives inline must be copied into
storage that outlives the sink call and stays alive until libzim pulls it. That is the
origin of the content-mode split in §4.2 and the buffering limit in §9.

## 3. Command surface

```sql
COPY <query-or-table> TO '<path>.zim' (FORMAT zim [, option …]);
```

### 3.1 Options

| Option | Type | Default | Meaning |
|---|---|---|---|
| `TITLE` | VARCHAR | — | `Title` metadata |
| `DESCRIPTION` | VARCHAR | — | `Description` metadata |
| `LANGUAGE` | VARCHAR | — | `Language` metadata (ISO 639-3, e.g. `eng`) |
| `CREATOR` | VARCHAR | — | `Creator` metadata |
| `PUBLISHER` | VARCHAR | — | `Publisher` metadata |
| `NAME` | VARCHAR | — | `Name` metadata |
| `DATE` | VARCHAR | — | `Date` metadata (`YYYY-MM-DD`) |
| `TAGS` | VARCHAR | — | `Tags` metadata |
| `METADATA` | MAP(VARCHAR, VARCHAR) | `{}` | arbitrary additional metadata keys |
| `ILLUSTRATION` | BLOB | — | 48×48 PNG cover image |
| `MAIN_PATH` | VARCHAR | — | landing page; **must match an item in the input** (§7.3) |
| `INDEX` | BOOLEAN | `false` | build a Xapian fulltext index |
| `INDEX_LANGUAGE` | VARCHAR | `LANGUAGE` | stemming language for the index |
| `COMPRESSION` | VARCHAR | `zstd` | `zstd` \| `none` — see below |
| `CLUSTER_SIZE` | BIGINT | libzim default | target uncompressed cluster size in bytes |
| `WORKERS` | BIGINT | `4` | libzim's internal worker count |
| `ON_CONFLICT` | VARCHAR | `error` | duplicate-path policy: `error` \| `first`. `last` is parsed but rejected at bind time with an explanatory error (§7.1) |

Named metadata options are sugar for `METADATA`; `TITLE 'x'` and `METADATA {'Title':'x'}`
are the same thing. Specifying both for one key is an error rather than last-wins,
matching how the reader already rejects conflicting `read_zim` parameters.

**`COMPRESSION` offers only `zstd` and `none`, not the three this document first claimed.**
libzim 9.7.0 has removed LZMA entirely — `zim/zim.h` declares

```c
  enum class Compression
  {
    None = 1,
    // intermediate values correspond to compression
    // methods that are no longer supported
    Zstd = 5
  };
```

so there is no enumerator to reference. An `lzma` request must be rejected at bind time
with a message that says the format dropped it, rather than silently falling back to zstd —
a caller who asked for a specific compression and got a different one has been lied to.
(Found during implementation; the original three-value list here was written from the
`configCompression` signature without checking the enum.)

**No metadata is required.** Measured: an archive with no metadata at all is written
successfully by libzim and *served* by kiwix-serve 3.8.2 (HTTP 200, viewer renders). So
metadata options are genuinely optional and there is no validity gate to build. The one
caveat is that a library-*catalog* view may need `Title`/`Description` to display an entry
usefully even though plain serving works — untested, and the reason §7.4 emits a warning
rather than staying silent.

### 3.2 Options that must be rejected, not ignored

- `FILE_SIZE_BYTES` / file rotation — `copy_rotate_files` returns `false`. A ZIM cannot be
  split mid-write and remain valid.
- `USE_TMP_FILE` — the Creator owns its own output path.

Silently accepting either would produce output that does not match what was asked for,
which is the failure family this design is most concerned with.

### 3.3 `PARTITION_BY` — supported, in v2

Writing one archive per value of a derived column is genuinely useful (a corpus split by
language or subject), and DuckDB's operator already provides the right structure:
`copy_to_initialize_global` is called **per partition file** with its full path, and
`copy_to_finalize` per partition. That is exactly "one `Creator` per archive", so support
is mostly a matter of *not rejecting it*.

```sql
COPY (SELECT path, title, mimetype, content, lang FROM articles)
TO 'out' (FORMAT zim, PARTITION_BY (lang));
-- out/lang=eng/data_0.zim, out/lang=fra/data_0.zim, …
```

Two complications, both requiring deliberate handling rather than being fatal:

**A partition can fragment into several archives.** When the number of open partitions
reaches `partitioned_write_max_open_files` (default 100), DuckDB finalises and evicts one,
then reopens that key later as `data_1.zim`. For parquet this is invisible — the files are
just more row groups. For a ZIM it is not: each fragment is a separate self-contained
archive needing its own metadata, its own fulltext index, and its own main page. A user who
asked for "one archive per language" and silently received three has a fragmented corpus.
**Warn when a partition is reopened**, naming the key.

**`MAIN_PATH` does not survive partitioning.** The landing page exists in exactly one
partition, so §7.3's validation would fail for every other one. **Reject `MAIN_PATH`
together with `PARTITION_BY`** rather than letting it half-work.

Metadata options apply identically to every partition. That is defensible, and templating
(`TITLE 'Wikipedia ({lang})'`) is left out until someone asks for it.

The no-overwrite rule of §3.4 composes unchanged: it is checked per output file, so each
partition file must not already exist.

**The partition column does not collide with §4's "unknown columns are an error" rule.**
`write_partition_columns` defaults to `false`, and DuckDB strips partition columns from both
the names/types passed to `copy_to_bind` and the chunks passed to `copy_to_sink`
(`GetNamesWithoutPartitions` / `SetDataWithoutPartitions`). So `lang` above is never seen by
this function. If a user explicitly sets `write_partition_columns := true` the column *does*
arrive and will be rejected as unknown — correct, since a ZIM entry has nowhere to store it,
but the error must name the option so the cause is obvious.

### 3.4 No overwrite

**If the output path already exists, the copy fails.** This is a deliberate deviation from
DuckDB's convention — `parquet` and `csv` clobber by default — and should be documented as
a deviation rather than left looking like an oversight.

The justification is specific to this format. A ZIM is frequently the only copy of a corpus
that took hours to build, and (§7.2) a failed write leaves a *valid, checksummed,
`zim_check`-passing* archive. Clobber-by-default combined with silent-partial-success is a
combination that destroys data and then reports health.

Refusing to overwrite also **subsumes the self-reference hazard**:

```sql
-- would truncate the source while reading it
COPY (SELECT … FROM read_zim('a.zim')) TO 'a.zim' (FORMAT zim);
```

Because a source archive necessarily exists, it can never be a valid output path. This is
strictly better than comparing resolved real paths against every source, which symlinks and
relative paths make unreliable.

An explicit `OVERWRITE true` opt-in is the right shape if it is ever wanted. Nothing needs
it today, and the default must never be destruction.

## 4. Input schema

Columns are resolved **by name**, case-insensitively. Unknown columns are an error, not
ignored — a typo'd `titel` should not silently produce untitled entries.

### 4.1 Columns

| Column | Type | Required | Meaning |
|---|---|---|---|
| `path` | VARCHAR | **yes** | entry path; must be unique (§7.1) |
| `content` | BLOB or VARCHAR | see §4.2 | inline content |
| `title` | VARCHAR | no | defaults to `path` |
| `mimetype` | VARCHAR | no | defaults by content column type (§4.3) |
| `entry_kind` | VARCHAR | no | `item` (default) \| `redirect` \| `alias` — **v2** |
| `target` | VARCHAR | no | target path for `redirect`/`alias` — **v2** |
| `front_article` | BOOLEAN | no | libzim `FRONT_ARTICLE` hint |
| `compress` | BOOLEAN | no | libzim `COMPRESS` hint |
| `content_path` | VARCHAR | no | content *locator* — a file path or a `zim://` URI (§4.2) — **v2** |

### 4.2 Content: two columns, three modes

Content arrives one of two ways, and **exactly one of `content` / `content_path` must be
non-NULL** per row. Supplying both, or neither, is an error naming the row's `path`.

| Column | Holds | Meaning |
|---|---|---|
| `content` | BLOB or VARCHAR | the bytes themselves |
| `content_path` | VARCHAR | *where to get the bytes* |

`content_path` is a **locator**, and its scheme selects the mechanism:

| Locator | Mechanism | Buffering | Use |
|---|---|---|---|
| `content` column (inline) | `StringItem` | full, until libzim pulls | derived/generated content, text |
| `photo.png` (v2) | libzim `FileProvider` | none | packing files from disk |
| `zim://big.zim/A/Article` (v2) | lazy pull from the source archive | none | subset / augment an existing ZIM |

This deliberately reuses the grammar the extension already ships and documents
(`docs/filesystem.md`, `zim://<archive>.zim/<content-path>`), which is what lets three
modes live in two columns:

- The source mode's `(archive, entry)` pair is **already a single string** in that grammar,
  so it needs no second column and no delimiter packing — and paths, which may contain
  almost anything, never have to survive a round trip through an ad-hoc encoding.
- "File on disk" and "entry in another ZIM" are both just *a place to get bytes*, so they
  differ only by scheme. No mode enum is needed; the locator is self-describing.
- `content` keeps its own column, so its **SQL type is preserved** — which is what drives
  mimetype defaulting (§4.3). Folding it into a single payload column would force one type
  across all three modes and lose that.

Rejected alternatives, for the record: four columns (`content`, `content_path`,
`source_archive`, `source_entry`) is more explicit but has strictly more illegal states to
validate; a per-row *mode-enum column* plus one payload column is uniform on paper but
forces the source pair into a packed string and collapses BLOB vs VARCHAR. (Not to be
confused with the reader's `content_mode` **parameter** in §6.2 — that is a per-query
setting on the read side, not a per-row column on the write side.)

> **The surface reuses the `zim://` grammar; the implementation must not reuse the VFS.**
> The registered `zim://` filesystem materialises each entry into RAM on open (issue #27),
> which is exactly the buffering this mode exists to avoid. `COPY` parses the URI itself and
> hands libzim its own lazy provider over a pooled source archive. The grammar is a naming
> convention here, not a code path.

Non-local schemes in `content_path` (`s3://`, `http://`) are **rejected** rather than
silently buffered; libzim's `FileProvider` needs a local file, and pretending otherwise
would reintroduce unbounded buffering under a name that promises streaming.

`entry_kind` in (`redirect`, `alias`) requires `target` and forbids every content column.

### 4.3 Mimetype defaulting

If `mimetype` is absent or NULL, default from the **content column's SQL type**:
`VARCHAR → text/plain`, `BLOB → application/octet-stream`.

This is not cosmetic. libzim's `creator.cpp:272` writes `WARNING: mimetype missing for
<path>` to **stderr per row** — on a 15,000-entry archive that is 15,000 lines of stderr,
which is its own kind of unusable. Defaulting means it never fires.

Deriving the default from the column type is a genuine advantage DuckDB's type system
gives us over a python harness: passing text where binary belongs becomes a *type*
distinction rather than a silent size regression. (Measured in python-libzim: routing
binary content through a `str` re-encoded it as UTF-8 and inflated a 16.8 MB PDF to
25.0 MB — 49%. A `BLOB` column cannot make that mistake.)

> **The same hazard exists on the option path, and it bit during implementation.** DuckDB's
> `Value::ToString()` on a `BLOB` does not return the bytes — it invokes `CastFromBlob`
> (`duckdb/src/common/operator/cast_operators.cpp:1388`), which escapes every non-ASCII byte
> as a four-character `\xHH` sequence. Any binary-valued option — `ILLUSTRATION` today, any
> future one — must extract raw bytes (`StringValue::Get`, or the `FlatVector` /
> `string_t::GetString()` path the sink uses) and never `ToString()`.
>
> This is invisible to an ASCII test payload, which is why the test table above requires the
> illustration fixture to contain bytes ≥ 0x80. The failure is silent and the archive still
> writes; only the image is wrong.

## 5. Execution model

| Hook | Responsibility |
|---|---|
| `copy_to_bind` | validate options; resolve column indices by name; reject unknown columns and conflicting modes |
| `execution_mode` | always `REGULAR_COPY_TO_FILE` — **serial**; libzim parallelises internally via `configNbWorkers` |
| `copy_to_initialize_global` | fail if the output exists (§3.4); construct `Creator`; `config*()`; `startZimCreation()`; write metadata and illustration |
| `copy_to_sink` | for each row: validate, materialise content into owned storage, `addItem()` |
| `copy_to_finalize` | `setMainPath()`; `finishZimCreation()`; mark the global state *finished* |
| `~GlobalState` | if not *finished*, unlink the output (§7.2) |

The sink is serial because a single `Creator` is not documented as safe for concurrent
`addItem()`, and because entry ordering affects nothing we need. Parallelism comes from
libzim's own workers, which is where it belongs.

Metadata is written at global-init rather than finalize because `addMetadata()` is
confirmed to work any time after `startZimCreation()`, and writing it early means a failed
content stream cannot produce an archive with content but no identity.

## 6. Interaction with the reader

### 6.0 The round trip does not currently type-check

The claim in §1 — that `read_zim`'s output schema is `COPY`'s input schema — is **not true
of the reader as it stands**, and this is the gap to close:

| | columns |
|---|---|
| reader emits | `path, title, mimetype, is_redirect, redirect_path, size, content [, file_path]` |
| writer accepts | `path, title, mimetype, content, content_path, entry_kind, target, front_article, compress` |
| overlap | `path, title, mimetype, content` |

`COPY (FROM read_zim('a.zim')) TO 'b.zim' (FORMAT zim)` would fail at bind on
`is_redirect`. Two complementary changes close it, on opposite sides.

### 6.1 Writer side — tolerate the reader's native columns

The writer recognises the reader's spelling and maps it, rather than erroring:

| reader column | writer treatment |
|---|---|
| `is_redirect` + `redirect_path` | mapped to `entry_kind` / `target` |
| `size` | ignored — libzim derives it |
| `file_path` | ignored — provenance, not content |
| anything else unrecognised | **still an error** |

This is a fixed, known list, so it does not weaken the typo guard: `titel` still fails.
Supplying both spellings (`is_redirect` *and* `entry_kind`) is an error rather than
last-wins, consistent with §3.1.

Note the two spellings are **not equivalent**, which is why the writer keeps the enum as its
own contract: `is_redirect BOOLEAN` + `redirect_path` cannot express an *alias*. Adding an
`is_alias BOOLEAN` beside `is_redirect` would reintroduce exactly the illegal state
(`is_redirect AND is_alias`) that the enum exists to prevent. The reader's schema is
strictly less expressive, so the mapping is one-way: reader → `item`/`redirect` only.

### 6.2 Reader side — `content_mode`

Content is the dimension that actually differs between reading *to look at* and reading
*to write back*, so that is what the parameter names — not the destination.

`include_content` conflates two independent questions. Splitting them is what makes `smart`
definable rather than a vibe:

| dimension | parameter | question |
|---|---|---|
| **how** content is delivered | `content_mode` | inline bytes, a lazy reference, or both? |
| **which** entries get content | `include_content_types` | which entries, by type and size? |

#### `content_mode` — how

| Mode | Content columns emitted | Reads content? | Use |
|---|---|---|---|
| `none` (default) | `content`, always NULL | **no** | listing scans — today's default |
| `inline` | `content` | for the selected entries | today's `include_content := true` |
| `reference` | `content_path` | **no** | subset / augment — lazy by construction |
| `smart` | `content` **and** `content_path` | for the selected entries only | mixed corpora |

`reference` emits `zim://<archive>/<path>` per row, so a subset needs no string
concatenation and materialises nothing:

```sql
COPY (SELECT * FROM read_zim('big.zim', content_mode := 'reference')
      WHERE path LIKE 'science/%')
TO 'subset.zim' (FORMAT zim);
```

`smart` emits both columns with **exactly one non-NULL per row** — which is precisely the
writer's rule from §4.2, so the two halves meet without an adapter. Selected entries get
inline bytes; everything else gets a reference. It is the augment case: convert the text,
reference the binaries, materialise nothing you did not ask for.

#### `include_content_types` — which

Polymorphic, in the same spirit as `content_path`'s scheme dispatch: the simple cases stay
short, and the expressive case needs no second parameter.

| Value | Meaning |
|---|---|
| `BOOLEAN` | `true` = all entries, `false` = none |
| `VARCHAR` | one Accept-style mimetype pattern (`'text/html'`, `'image/*'`, `'*/*'`) |
| `LIST(VARCHAR)` | any of several patterns |
| `LIST(STRUCT(mimetype VARCHAR, min_size BIGINT, max_size BIGINT))` | patterns with size bounds |

```sql
-- text inline, everything else referenced and never materialised
read_zim(x, content_mode := 'smart', include_content_types := 'text/*');

-- inline small text and small PDFs; reference big ones
read_zim(x, content_mode := 'smart',
            include_content_types := [{'mimetype': 'text/*',        'max_size': 1048576},
                                      {'mimetype': 'application/pdf','max_size':   65536}]);
```

`min_size` / `max_size` are inclusive bounds in bytes; `NULL` means unbounded. They are
`BIGINT` rather than `INT` because a ZIM entry can exceed 2 GB, and `NULL`-able because
DuckDB types a `LIST(STRUCT)` uniformly — every element must carry the same keys, so an
unused bound is written as `NULL` rather than omitted.

Folding size into the selector, rather than adding a separate `content_inline_max_size`,
means there is no second knob that can disagree with the first, and per-type thresholds
become expressible — which a single global threshold cannot do.

**Default**: absent, `include_content_types` selects *all* entries, so `content_mode :=
'inline'` inlines everything (matching today's `include_content := true`). For
`content_mode := 'smart'` with no selector, the default is **textual mimetypes inline,
everything else referenced**, reusing the `LooksLikeText()` predicate already in
`src/zim_scalars.cpp`. That is a heuristic, and it is the one place in this design where a
default guesses at intent — say so in the docs rather than letting it be discovered.

#### Deprecation and defaults

`include_content` remains as a **deprecated alias**, mapped mechanically. It is published in
community extensions, so removing it would break working queries:

| today | becomes |
|---|---|
| `include_content := false` | `content_mode := 'none'` *(the default)* |
| `include_content := true` | `content_mode := 'inline'` |
| `include_content := 'text/html'` | `content_mode := 'inline', include_content_types := 'text/html'` |

Combining `include_content` with either new parameter is an error rather than last-wins,
consistent with §3.1 and with how the reader already rejects conflicting `read_zim`
parameters.

`content_mode := 'none'` is the default, preserving today's behaviour exactly: the reader is
released, and changing the shape or cost of an existing query's result is not something a
new feature gets to do.

`content_as_varchar` stays a **separate** parameter. It is a *type* decision (BLOB vs
VARCHAR) orthogonal to both dimensions above; folding it in would make `content_mode` a
cross-product enum.

> **Do not confuse `include_content_types` with the existing `mimetype` parameter.** They
> both take Accept-style patterns and they do different things: `mimetype` filters **which
> rows appear**; `include_content_types` filters **which of those rows carry content**. A
> row excluded by `mimetype` is absent; a row excluded by `include_content_types` is present
> with NULL content. This is a genuine footgun and the reference docs must show both in one
> example.

Redirect rows carry no content in any mode: both content columns are NULL and
`is_redirect`/`redirect_path` describe the entry.

### 6.3 What carries over

Three things already in the extension carry over and should not be rebuilt:

- **The `zim://` grammar** — §4.2 reuses it verbatim as the source-mode locator, so the URI
  format needs no new design and is already documented for users. Only the *parser* is
  shared; the VFS itself is deliberately bypassed (§4.2).
- **`ArchivePool`** — v2's source mode must open each distinct source archive once and keep
  it open until `finishZimCreation()` returns, because libzim pulls *after* the sink has
  moved on. Cache by resolved path; never close on a chunk boundary.
- **`read_zim(…, include_filepath := true)`** — already tags every row with its source
  archive, and the LIST overload already scans many archives in one call. v2's subset and
  union cases need no new reader work; the linchpin exists.

A **`zim_uri(archive, path)`** scalar would make the subset idiom read better than string
concatenation (`'zim://' || file_path || '/' || path`) and would centralise escaping. Worth
adding with v2, not before — it is sugar over a grammar that must work either way.

## 7. Failure modes

These are the parts worth building carefully. Each is measured, not hypothesised.

### 7.1 Duplicate paths

libzim throws `InvalidEntry` mid-stream on a duplicate path. Because `COPY` consumes
arbitrary query output, a duplicate path is not exotic — it is one careless `GROUP BY`, or
one `UNION ALL` of two archives that share a path, away. v2's subset/augment mode makes it
substantially more likely.

**Detect duplicates in the sink**, before handing the path to libzim, keeping a set of
seen paths. This gives an error that names the SQL problem rather than surfacing a libzim
internal, and it makes `ON_CONFLICT` implementable:

- `error` (default) — fail, naming the offending path
- `first` — keep the first occurrence, skip later ones
- `last` — cannot be done streaming without buffering every row; **rejected at bind time**
  with an explanatory error rather than silently buffering

The memory cost is one copy of every path. On a 15,000-entry archive that is negligible;
on a pathological one it is bounded by the path set, not the content.

### 7.2 A *finalized* failed write leaves a healthy-looking archive

> **Corrected during implementation.** This section originally claimed that any mid-stream
> failure leaves a valid-looking archive at the output path, and called that the most
> important finding in the document. That is **not true of this implementation**, and the
> claim was built on a measurement of a different mechanism. The hazard is real but
> narrower than stated. Both the original measurement and the correction are kept below,
> because the reconciliation is the useful part.

**What was measured (python-libzim):** after a duplicate-path throw mid-stream, a
35,069-byte file at the output path was *not* litter — it was a valid archive.

| check | result |
|---|---|
| `zim::Archive` opens it | yes, `entry_count=1` |
| `read_zim` opens it | yes, returns the row |
| `zim_info().has_checksum` | **true** |
| `zim_check()` | **true** |

Two independent readers accept it and the integrity check passes, because `zim_check`
verifies internal *consistency*, not *completeness*, and the ZIM format records no expected
entry count. That part stands, and §7.2's warning about `zim_check` remains correct.

**What actually produced it.** libzim never writes the target name incrementally. In 9.7.0,
`src/writer/creator.cpp:579` sets `tmpFileName(fname + ".tmp")`, and `:453` performs
`DEFAULTFS::rename(data->tmpFileName, data->zimName)` at the *end* of
`finishZimCreation()`. The final name therefore appears only on success.

The measured file existed because python-libzim's `Creator` context manager calls
`finishZimCreation()` from `__exit__` **even while an exception is propagating** — so what
was measured was a *finished truncated* archive, not a partial one. Every observed property
follows from that. The two results do not conflict; they measured different mechanisms.

**Consequence for this design.** `COPY` never finalizes on the error path, so no
SQL-reachable abort creates a file at the target name at all. Therefore:

- **Never call `finishZimCreation()` from an error path.** This is the actual guarantee.
  A truncated-but-finalized archive is indistinguishable from a complete one, and no
  post-hoc validation can catch it — the obvious validator says *fine*.
- **Unlink the output on any error anyway**, from the global state's destructor guarded by a
  `finished` flag. This is **defense in depth, not the load-bearing mechanism**: it costs
  nothing, and it stays correct if a future libzim drops the rename, or if a future code
  path creates the target early.
- **Do not manufacture a test for the unlink.** With no reachable trigger, any such test
  would assert against a scenario the code cannot reach. The path is deliberately untested
  and labelled as such — an honest gap beats a test that only appears to cover something.

§3.4's no-overwrite rule is what makes recovery clean regardless: if the output could not
have existed beforehand, then whether the abort leaves nothing (the normal case) or leaves
something the destructor removes, the prior state is restored exactly.

**The `.tmp` sibling.** An aborted write leaves libzim's `<target>.tmp` to libzim's own
destructor. Task 2's implementer confirmed by `strace` that it is removed. If that ever
proves untrue, the fix is to unlink `<target>.tmp` alongside the target — noted here so the
next person does not have to re-derive it.

### 7.3 Invalid `MAIN_PATH` is silent in libzim

Measured: `setMainPath()` pointing at a path that was never added completes without error
and yields an archive with `has_main_entry = false`. No throw, no warning.

So a `MAIN_PATH` naming a page the query never produced would silently produce a working
archive with no landing page. **Validate it in `copy_to_finalize`** against the set of
paths actually written, and error.

### 7.4 Dangling redirects are silently removed — so v1 rejects them

Measured: `addRedirection()` to a nonexistent target completes without error. libzim
detects and *removes* invalid redirections at `finishZimCreation()`, announcing each only
via the stdout chatter §7.5 exists to suppress.

Suppressing that output would therefore destroy the only signal that entries were dropped,
and **silently dropping entries is strictly worse than the pollution.**

> **This was scoped to v2 on a premise that stopped being true, and the two halves nearly
> met badly.** The reasoning was "v1 cannot write redirects, so this is unreachable." That
> held until v1 gained the writer-tolerance of `read_zim`'s `is_redirect` / `redirect_path`
> columns (§6.1) — which is exactly what makes the round trip work, and which also made
> dangling redirects reachable. Had §7.5's patch landed on top of that without anyone
> noticing, entries would have vanished from archives with no signal anywhere: two
> individually-correct changes producing a silent data loss neither one introduced.

**v1 therefore validates redirect targets rather than deferring.** `copy_to_finalize`
checks every non-empty `redirect_path` against the set of paths actually written, before
`finishZimCreation()`, and errors naming the offending redirect and its missing target.
This mirrors §7.3's identical treatment of `MAIN_PATH`, costs one set, and needs no
ordering discipline from the caller — finalize runs after every row has been seen, so a
redirect declared before its target is fine.

Consequence worth stating: the two removal `INFO()` sites are now **unreachable from SQL by
construction**, because the archive is rejected before libzim can drop anything. That the
patch covers them is confirmed by reading the patched source rather than by triggering
them — and being unable to trigger them is the better state, not a test gap.

If a future version wants to *accept* dangling redirects (mirroring libzim's own tolerance),
the removals must come back as a DuckDB warning or a rejected-row count first. Accepting
them silently is not an option this design will take.

### 7.5 libzim writes to stdout, and `configVerbose(false)` does not stop it

In 9.7.0, `src/writer/creator.cpp` gates two of its three logging macros and not the third:

```c
#define INFO(e)     do { log_info(e); std::cout << e << std::endl; } while(false)   // NOT gated
#define TINFO(e)    if (m_verbose) { … }                                            // gated
#define TPROGRESS() if (m_verbose) { … }                                            // gated
```

`INFO()` is called at 6 sites. Measured with `configVerbose(false)` and the streams
captured separately, four fire on a normal write, all on **stdout**, none on stderr:

```
Detect dangling redirects
Detect loops and/or blind chains of redirects
Index titles
Set entry indices
```

The other two are the redirection-removal messages of §7.4, which only fire when a redirect
is actually dangling. Because §7.4's validation now rejects such an archive before libzim
sees it, those two sites are **unreachable from SQL**. That the patch silences them is
verified by reading the patched source — and it is verifiable at all only because the patch
targets the `INFO` macro *definition* rather than its six call sites, so one hunk covers
every site including the ones no test can reach.

In a DuckDB CLI this is output corruption; in an MCP stdio server sharing stdout with
JSON-RPC it is protocol corruption. The fix is a patch in the existing overlay port
(`vcpkg_ports/libzim/`), which already carries `no-stemming-stdout.patch` for exactly this
class of problem — precedent and machinery both exist.

### 7.6 Version skew

The string set above is version-specific. A python-libzim 3.12.0 wheel emits six strings
including `Adding checksum...` and `ZIM file is ready!`, neither of which exists anywhere in
9.7.0's source. Patch against our own tree; treat any external string list as evidence that
the set varies, never as a target.

## 8. Round-trip conformance, and where it is not an identity

`COPY (SELECT path, title, mimetype, content FROM read_zim(x)) TO y (FORMAT zim)` is the
natural conformance test, and a 10-entry real-Wikipedia round trip has been measured
byte-identical, 10/10.

**It is not an identity for aliases**, and the way it fails is quiet. An alias read back
through `read_zim` is indistinguishable from a normal item — same `is_redirect = false`,
same mimetype, same size, `redirect_path` NULL — so writing it back produces a full
duplicate item rather than an alias. The archive stays semantically equivalent to a reader
but gains a second copy of the data and a Counter that differs.

A "content matches" assertion passes in exactly that case. **The round-trip test must
assert `entry_count`, `all_entry_count` and `zim_counter()` alongside content**, or it is a
check that cannot fail where the round trip is not an identity.

> **Read the source as `BLOB`, never with `content_as_varchar := true`.** This looks like a
> convenience and is actually silent data loss. `content_as_varchar` returns `NULL` for any
> entry whose bytes are not valid UTF-8 — which is every image, font and media file in a real
> archive — so a copy driven by it writes **empty content** for exactly those entries, and a
> content-comparison test still passes because `NULL = NULL` on both sides.
>
> Found during implementation: the round-trip test as originally specified read the fixture
> with `content_as_varchar := true`, and the fixture's binary `I/logo.png` would have been
> copied as an empty entry with the test reporting success. The rule generalises beyond the
> test — **any** ZIM→ZIM copy must move content as `BLOB`. The `NULL`-not-mangled behaviour is
> correct for *reading* text (it is what keeps `zim_get_text` honest); it is catastrophic for
> *copying*, because the copy has no way to tell "this entry was empty" from "this entry was
> binary and I dropped it".

### 8.1 Aliases can be preserved — via a private API

An alias shares its target's stored data, so two entries with the same
`(clusterIndex, blobIndex)` are the same stored blob. That is the alias relationship, per
row. Measured on 9.7.0:

```
path                kind      index   cluster   blob
alias_of_target     item      0       0         2
dup_a               item      1       0         0
dup_b               item      2       0         1
redir_to_target     redirect  3       -         -
target              item      4       0         2
```

`dup_a` and `dup_b` have **byte-identical content** and got **different blobs**, so the
**Creator does not deduplicate** and the signal is *exact*, not merely necessary.

Three constraints on using it:

1. **`getClusterIndex()`/`getBlobIndex()` are behind `#ifdef ZIM_PRIVATE`.** They link
   (the symbols are exported), but this is not public API. A libzim release could remove
   them — a loud link error — or change their semantics — a *silent* corruption that turns
   distinct items into aliases. The pinned-version test must therefore assert the
   **property, not the symbol**: write two byte-identical items at different paths and
   assert their pairs *differ*; write an alias and assert its pair *matches* its target.
2. **Entry index does not identify the original.** Indices are assigned in path-sort order,
   not insertion order — above, the alias is index 0 and its target index 4. Any
   "lowest index is the original" rule inverts every alias. Within a shared-blob group the
   choice is arbitrary and unrecoverable from the format; pick deterministically (lowest
   path) and stop. The result is a faithful reproduction rather than a byte-identical one,
   and nothing downstream can tell the difference.
3. **`getDirectAccessInformation()` is not a public substitute.** It returned invalid for
   every item in the probe, because direct access exists only for uncompressed clusters and
   compression is the point of the format.

Surfacing this to SQL needs a reader change (optional `cluster_index`/`blob_index` columns
on `read_zim`), so it is **v2 or later**. v1 documents the alias non-identity.

## 9. Stated non-goals

- **ZIM→ZIM transform of large binaries.** Content that comes from a query cannot be a
  file path and may be too large to buffer. Workaround: spill to temp files and use
  `content_path` (v2). Documented rather than left to be discovered.
- **Concurrent writers to one archive.** Untested, unsupported.
- **`COPY FROM`** — reading is already `read_zim`'s job.
- **Appending to an existing archive.** libzim's `Creator` writes a new file; §3.4 forbids
  targeting an existing one. Augmenting is done by reading the old archive and writing a
  new one (v2's source mode).

## 10. Scope

**v1** — items only, inline content only. Columns `path`, `content`, `title`, `mimetype`,
`front_article`, `compress`; all options in §3.1; §7.1 through §7.5 all handled — including
§7.4, which was originally deferred on a premise that writer tolerance then invalidated.
**Writer tolerance (§6.1) is v1**, because it is what makes the
naive round trip work and the conformance test exist at all; `is_redirect` maps to a
redirect entry even though `entry_kind` is not yet a column the user can write.
`content_path` and `PARTITION_BY` are rejected with "not yet supported" rather than
"unknown", so the deferral reads as deferral.

**v2, immediately following** — `content_path` in both its forms (local file, and the
`zim://` locator driving the subset/augment use cases), the reader's `content_mode` /
`include_content_types` rework (§6.2),
`entry_kind`/`target`, §7.4, and `PARTITION_BY` (§3.3). The v1 column contract is unchanged
by all of this: v2 only adds columns, never renames or retypes one.

**Later** — alias preservation (§8.1), contingent on reader changes; `zim_uri()` sugar (§6);
per-partition metadata templating (§3.3).

### 10.1 Test plan

Every item below is a regression test, not a manual check.

| Test | Asserts |
|---|---|
| minimal write | a one-item archive opens and `read_zim` returns the row |
| round trip | content **and** `entry_count`, `all_entry_count`, `zim_counter()` (§8) |
| round trip of a **binary** entry | content compared as `BLOB`; must fail if binary entries are dropped. Reading the source with `content_as_varchar := true` silently NULLs non-UTF-8 entries and the comparison still passes (§8) |
| metadata | every option in §3.1 reads back via `zim_metadata` |
| `ILLUSTRATION` byte fidelity | the stored PNG reads back **byte-identical** via `zim_illustration`, and `octet_length` matches. The payload **must contain bytes ≥ 0x80** — DuckDB's `Value::ToString()` on a `BLOB` escapes non-ASCII as `\xHH`, so an ASCII-only payload passes even when the extraction is broken (§4.3) |
| `METADATA` with a non-VARCHAR value | rejected, not silently escaped through the same `ToString()` path |
| mimetype defaulting | `VARCHAR → text/plain`, `BLOB → application/octet-stream` |
| duplicate path, `error` | fails, error names the path, **output does not exist** (§7.2) |
| duplicate path, `first` | first occurrence wins, later skipped |
| `ON_CONFLICT last` | rejected at bind time |
| existing output | refused; **the existing file is unmodified** (§3.4) |
| mid-stream error | **no file at the output path** — asserted with `glob()`, which means exactly "absent" where `zim_check() = false` would conflate absent with corrupt (§7.2) |
| error path never finalizes | no code path calls `finishZimCreation()` while unwinding — the actual §7.2 guarantee |
| invalid `MAIN_PATH` | errors rather than writing a mainless archive (§7.3) |
| `FILE_SIZE_BYTES` | rejected, not ignored (§3.2) |
| `content` and `content_path` both set | error naming the row's `path` (§4.2) |
| neither content column set | error naming the row's `path` (§4.2) |
| naive round trip | `COPY (FROM read_zim(x)) TO y` **binds and succeeds** (§6.1) |
| `size` / `file_path` present | ignored, not an error (§6.1) |
| `is_redirect` + `redirect_path` | mapped to a redirect entry (§6.1) |
| both spellings supplied | `is_redirect` *and* `entry_kind` together is an error (§6.1) |
| a genuinely unknown column | still an error — `titel` fails (§6.1) |
| `INDEX true` | `zim_search` finds an entry in the written archive |
| `INDEX true` without a language | clear error |
| `INDEX true` on WASM | clear error — Xapian is absent, must not silently no-op |
| stdout cleanliness | a successful write emits **nothing** on stdout; the test must also assert the write actually happened, or the assertion is vacuous (§7.5) |
| dangling redirect | rejected naming the offending redirect and its missing target, with **no output file surviving** (§7.4) |
| libzim private-API property | the §8.1 dedup/alias property holds on the pinned version |

v2 adds:

| Test | Asserts |
|---|---|
| `content_path` from disk | bytes match the source file; no buffering of the whole file |
| `zim://` locator | subset of a source archive round-trips; **content is pulled lazily**, i.e. no read happens during the sink |
| `content_mode := 'reference'` | emits `content_path`, not `content`; **reads no content at all** (§6.2) |
| `content_mode := 'smart'` | emits both columns, exactly one non-NULL per row (§6.2) |
| `content_mode` default | absent parameter emits today's schema unchanged — no existing query shifts (§6.2) |
| `include_content_types` scalar / list | mimetype patterns select which rows carry content (§6.2) |
| `include_content_types` struct form | `min_size`/`max_size` bounds honoured; `NULL` means unbounded (§6.2) |
| per-type thresholds | two struct rules with different `max_size` each apply to their own type |
| `smart` with no selector | textual mimetypes inline, everything else referenced (§6.2) |
| `include_content` alias | all three old spellings still work and map as documented (§6.2) |
| old + new spelling together | error, not last-wins (§6.2) |
| `mimetype` vs `include_content_types` | a row excluded by `mimetype` is **absent**; one excluded by `include_content_types` is **present with NULL content** (§6.2) |
| redirect row in every mode | both content columns NULL (§6.2) |
| `zim://` to a missing entry | clear error naming the URI |
| non-local scheme in `content_path` | rejected (§4.2), not silently buffered |
| `PARTITION_BY` | one archive per key, each independently openable by `read_zim` |
| `PARTITION_BY` + `MAIN_PATH` | rejected together (§3.3) |
| partition fragmentation | a key reopened past `partitioned_write_max_open_files` warns, naming the key (§3.3) |
| `write_partition_columns := true` | error names the option, not just "unknown column" (§3.3) |
