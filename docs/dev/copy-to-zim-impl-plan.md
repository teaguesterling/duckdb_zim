# `COPY … TO … (FORMAT zim)` v1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `COPY (query) TO 'out.zim' (FORMAT zim, …)` copy function that writes a
valid, optionally-searchable ZIM archive from any DuckDB relation.

**Architecture:** Two new translation units, mirroring the split the reader already uses.
`src/zim_writer.{hpp,cpp}` holds **all** `zim::writer::*` contact and exposes only plain
structs; `src/copy_to_zim.cpp` is the DuckDB `CopyFunction` binding and never names a
`zim::` type. The sink is serial (`REGULAR_COPY_TO_FILE`) because a single `Creator` is not
documented as concurrency-safe; parallelism comes from libzim's own worker threads.

**Tech Stack:** C++17, DuckDB extension API (`ExtensionLoader`, `CopyFunction`), libzim
9.7.0 via a vcpkg overlay port, sqllogictest + one bash script for stdout assertions.

**Spec:** `docs/dev/copy-to-zim-design.md` — read it first. This plan implements **v1 only**
(§10). Every design decision below is argued there; this document does not re-argue them.

## Global Constraints

- **All libzim contact stays in `src/zim_writer.cpp`** (plus the existing `zim_access.cpp` /
  `zim_archive_pool.cpp`). If `copy_to_zim.cpp` needs a libzim concept, add a method to
  `ZimWriter` instead. This is the codebase's established rule — see the header comment in
  `src/zim_access.hpp`.
- **License GPL-2.0-or-later.** No new dependencies; no HTML/XML/markdown parsing.
- **C++17.** The build forces it regardless of DuckDB's cached `CMAKE_CXX_STANDARD=11`.
- **libzim is pinned at 9.7.0**, `vcpkg_ports/libzim/vcpkg.json`, currently port-version 2.
- **Naming mirrors the reader:** `zim_*` scalars, `read_zim*` table functions, `snake_case`
  copy options.
- **v1 is items only.** `content_path`, `entry_kind`, `target`, `PARTITION_BY`, and the
  reader's `content_mode` rework are **v2** and must be rejected with a "not yet supported"
  message, never silently ignored.
- **Formatting:** run `make format` before each commit; the repo has `.clang-format`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/zim_writer.hpp` (create) | Plain structs (`ZimWriteEntry`, `ZimWriterConfig`) + `ZimWriter` class declaration. No `zim::` types. |
| `src/zim_writer.cpp` (create) | The only place that includes `<zim/writer/*>`. Wraps `Creator` lifecycle. |
| `src/copy_to_zim.cpp` (create) | `CopyFunction` bind/init/sink/finalize, option parsing, column resolution, duplicate detection, unlink-on-error. |
| `src/zim_extension.cpp` (modify) | Declare and call `RegisterCopyToZim(loader)`. |
| `CMakeLists.txt` (modify) | Add the two new sources to `EXTENSION_SOURCES`. |
| `vcpkg_ports/libzim/no-writer-stdout.patch` (create) | Silence the ungated `INFO()` `std::cout` writes. |
| `vcpkg_ports/libzim/portfile.cmake` (modify) | Apply the new patch. |
| `vcpkg_ports/libzim/vcpkg.json` (modify) | Bump `port-version` 2 → 3. |
| `test/sql/copy_zim.test` (create) | Happy paths: write, read back, round trip, metadata, index. |
| `test/sql/copy_zim_errors.test` (create) | Refusals: existing output, duplicate paths, bad options, unsupported v2 options. |
| `test/no_writer_stdout_pollution.sh` (create) | Asserts a write emits nothing on stdout. sqllogictest cannot check this. |
| `.github/workflows/MainDistributionPipeline.yml` (modify) | Run the new shell test. |
| `docs/reference.md`, `README.md` (modify) | Document the copy function. |

---

## Task 1: Minimal `COPY TO` — write items and read them back

**Files:**
- Create: `src/zim_writer.hpp`, `src/zim_writer.cpp`, `src/copy_to_zim.cpp`
- Modify: `CMakeLists.txt:23-32`, `src/zim_extension.cpp:18-23` and `:49-53`
- Test: `test/sql/copy_zim.test`

**Interfaces:**
- Produces: `duckdb::zim_ext::ZimWriteEntry`, `duckdb::zim_ext::ZimWriterConfig`,
  `duckdb::zim_ext::ZimWriter` (ctor, `AddItem`, `Finish`), and
  `duckdb::RegisterCopyToZim(ExtensionLoader &)`. Every later task builds on these exact
  names.

- [ ] **Step 1: Write the failing test**

Create `test/sql/copy_zim.test`:

```
# name: test/sql/copy_zim.test
# description: COPY ... TO ... (FORMAT zim) -- writing archives
# group: [sql]

require zim

statement ok
PRAGMA enable_verification

# --- minimal write: two items round-trip through read_zim -------------------
statement ok
COPY (SELECT * FROM (VALUES ('A/One', 'One', 'text/html', 'first'),
                            ('A/Two', 'Two', 'text/html', 'second'))
        t(path, title, mimetype, content))
TO '__TEST_DIR__/minimal.zim' (FORMAT zim);

query III
SELECT path, title, mimetype
FROM read_zim('__TEST_DIR__/minimal.zim')
ORDER BY path;
----
A/One	One	text/html
A/Two	Two	text/html

query I
SELECT content
FROM read_zim('__TEST_DIR__/minimal.zim', path := 'A/One',
              include_content := true, content_as_varchar := true);
----
first

# title defaults to path when the column is absent
statement ok
COPY (SELECT 'A/NoTitle' AS path, 'x' AS content)
TO '__TEST_DIR__/notitle.zim' (FORMAT zim);

query II
SELECT path, title FROM read_zim('__TEST_DIR__/notitle.zim');
----
A/NoTitle	A/NoTitle

# --- mimetype defaults from the CONTENT COLUMN'S TYPE -----------------------
# Not cosmetic: libzim writes "WARNING: mimetype missing for <path>" to stderr
# once PER ROW, so a 15,000-entry archive with no mimetype is 15,000 lines of
# stderr. Deriving the default from the SQL type is the advantage DuckDB's type
# system gives us -- text and bytes are distinguishable here.
statement ok
COPY (SELECT 'A/Text' AS path, 'plain words' AS content)
TO '__TEST_DIR__/deftext.zim' (FORMAT zim);

query I
SELECT mimetype FROM read_zim('__TEST_DIR__/deftext.zim');
----
text/plain

statement ok
COPY (SELECT 'A/Bytes' AS path, '\x00\x01\x02'::BLOB AS content)
TO '__TEST_DIR__/defblob.zim' (FORMAT zim);

query I
SELECT mimetype FROM read_zim('__TEST_DIR__/defblob.zim');
----
application/octet-stream
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim.test"
```

Expected: FAIL — `Copy Function with name "zim" does not exist` (or similar binder error).

- [ ] **Step 3: Write `src/zim_writer.hpp`**

```cpp
//===----------------------------------------------------------------------===//
// zim_writer.hpp
//
// Thin wrapper over libzim's writer, mirroring zim_access.hpp on the read side.
// All zim::writer contact for the extension is confined to zim_writer.cpp; the
// DuckDB binding layer (copy_to_zim.cpp) consumes only the plain structs below
// and never touches a zim:: type directly.
//
// Ownership note that drives the whole design: libzim's Creator is a PULL
// interface -- Item::getContentProvider() is called on libzim's own worker
// threads, long after addItem() returns. DuckDB's sink is PUSH and recycles its
// DataChunk vectors the moment it returns. So AddItem() must COPY content into
// storage libzim owns; zim::writer::StringItem does exactly that.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace duckdb {
namespace zim_ext {

// One entry to write. Field names mirror ZimEntry on the read side so the
// round trip reads as an identity.
struct ZimWriteEntry {
	std::string path;
	std::string title;
	std::string mimetype;
	std::string content;
	bool is_redirect = false;
	std::string redirect_path;
	// Hints are tri-state: absent means "let libzim decide from the mimetype".
	bool has_front_article = false;
	bool front_article = false;
	bool has_compress = false;
	bool compress = false;
};

// Archive-level settings, all optional. Defaults match libzim's own.
struct ZimWriterConfig {
	std::string compression = "zstd"; // zstd | lzma | none
	uint64_t cluster_size = 0;        // 0 = libzim default
	uint32_t workers = 4;
	bool index = false;
	std::string index_language;
	std::string main_path;
	std::map<std::string, std::string> metadata;
	std::string illustration; // raw 48x48 PNG bytes; empty = none
};

// RAII wrapper around zim::writer::Creator.
//
// Construction starts the archive (the output file is created immediately).
// Finish() completes it. If the object is destroyed WITHOUT a successful
// Finish(), the partial output is left on disk -- deleting it is the caller's
// job, because only the caller knows the path policy. See copy_to_zim.cpp.
class ZimWriter {
public:
	ZimWriter(const std::string &out_path, const ZimWriterConfig &config);
	~ZimWriter();
	ZimWriter(const ZimWriter &) = delete;
	ZimWriter &operator=(const ZimWriter &) = delete;

	// Throws on a duplicate path (libzim zim::InvalidEntry). The caller detects
	// duplicates first so the error names the SQL problem, not a libzim internal.
	void AddItem(const ZimWriteEntry &entry);

	// Applies main_path and metadata, then finalizes. After this returns the
	// archive on disk is complete and valid.
	void Finish();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace zim_ext
} // namespace duckdb
```

- [ ] **Step 4: Write `src/zim_writer.cpp`**

```cpp
//===----------------------------------------------------------------------===//
// zim_writer.cpp — the ONLY translation unit that includes <zim/writer/*>.
//===----------------------------------------------------------------------===//
#include "zim_writer.hpp"

#include <zim/writer/creator.h>
#include <zim/writer/item.h>
#include <zim/zim.h>

#include <stdexcept>

namespace duckdb {
namespace zim_ext {

namespace {

zim::Compression ParseCompression(const std::string &name) {
	if (name == "zstd") {
		return zim::Compression::Zstd;
	}
	if (name == "lzma") {
		return zim::Compression::Lzma;
	}
	if (name == "none") {
		return zim::Compression::None;
	}
	throw std::runtime_error("unknown compression '" + name + "'; expected zstd, lzma or none");
}

} // namespace

struct ZimWriter::Impl {
	zim::writer::Creator creator;
	ZimWriterConfig config;
};

ZimWriter::ZimWriter(const std::string &out_path, const ZimWriterConfig &config) : impl(new Impl()) {
	impl->config = config;
	impl->creator.configVerbose(false);
	impl->creator.configCompression(ParseCompression(config.compression));
	impl->creator.configNbWorkers(config.workers);
	if (config.cluster_size > 0) {
		impl->creator.configClusterSize(static_cast<zim::size_type>(config.cluster_size));
	}
	if (config.index) {
		impl->creator.configIndexing(true, config.index_language);
	}
	impl->creator.startZimCreation(out_path);
}

ZimWriter::~ZimWriter() = default;

void ZimWriter::AddItem(const ZimWriteEntry &entry) {
	zim::writer::Hints hints;
	if (entry.has_front_article) {
		hints[zim::writer::FRONT_ARTICLE] = entry.front_article ? 1 : 0;
	}
	if (entry.has_compress) {
		hints[zim::writer::COMPRESS] = entry.compress ? 1 : 0;
	}
	if (entry.is_redirect) {
		impl->creator.addRedirection(entry.path, entry.title, entry.redirect_path, hints);
		return;
	}
	// StringItem::create copies `content` into storage libzim owns, which is what
	// makes the pull/push mismatch safe (see the header comment).
	impl->creator.addItem(
	    zim::writer::StringItem::create(entry.path, entry.mimetype, entry.title, hints, entry.content));
}

void ZimWriter::Finish() {
	for (auto &kv : impl->config.metadata) {
		impl->creator.addMetadata(kv.first, kv.second);
	}
	if (!impl->config.illustration.empty()) {
		impl->creator.addIllustration(48, impl->config.illustration);
	}
	if (!impl->config.main_path.empty()) {
		impl->creator.setMainPath(impl->config.main_path);
	}
	impl->creator.finishZimCreation();
}

} // namespace zim_ext
} // namespace duckdb
```

- [ ] **Step 5: Write `src/copy_to_zim.cpp`**

```cpp
//===----------------------------------------------------------------------===//
// copy_to_zim.cpp — COPY ... TO ... (FORMAT zim).
//
// DuckDB binding only: no zim:: types appear here (see zim_writer.hpp).
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_writer.hpp"

#include <set>

namespace duckdb {

using zim_ext::ZimWriteEntry;
using zim_ext::ZimWriter;
using zim_ext::ZimWriterConfig;

namespace {

// Column indices resolved by name at bind time. -1 means "column absent".
struct ZimColumns {
	int64_t path = -1;
	int64_t content = -1;
	int64_t title = -1;
	int64_t mimetype = -1;
};

struct ZimCopyBindData : public FunctionData {
	ZimColumns cols;
	ZimWriterConfig config;
	// Default mimetype, derived from the content column's SQL type at bind time.
	string default_mimetype = "text/plain";

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ZimCopyBindData>();
		result->cols = cols;
		result->config = config;
		result->default_mimetype = default_mimetype;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		return this == &other_p;
	}
};

struct ZimCopyGlobalState : public GlobalFunctionData {
	ZimCopyGlobalState(ClientContext &context_p, string path_p)
	    : context(context_p), out_path(std::move(path_p)) {
	}

	~ZimCopyGlobalState() override {
		// The central guarantee: a COPY that did not reach finalize must leave no
		// output behind. A partial ZIM is NOT litter -- it opens, checksums, and
		// passes zim_check(), so leaving it would present a truncated corpus as a
		// healthy archive. See docs/dev/copy-to-zim-design.md §7.2.
		if (finished) {
			return;
		}
		writer.reset(); // close libzim's handle before unlinking
		try {
			auto &fs = FileSystem::GetFileSystem(context);
			if (fs.FileExists(out_path)) {
				fs.RemoveFile(out_path);
			}
		} catch (...) { // NOLINT: a destructor must not throw
		}
	}

	ClientContext &context;
	string out_path;
	unique_ptr<ZimWriter> writer;
	std::set<string> seen_paths;
	bool finished = false;
};

struct ZimCopyLocalState : public LocalFunctionData {};

// Resolve a column by name, case-insensitively. Returns -1 when absent.
int64_t FindColumn(const vector<string> &names, const string &want) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (StringUtil::CIEquals(names[i], want)) {
			return static_cast<int64_t>(i);
		}
	}
	return -1;
}

unique_ptr<FunctionData> ZimCopyBind(ClientContext &context, CopyFunctionBindInput &input,
                                     const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto bind = make_uniq<ZimCopyBindData>();

	bind->cols.path = FindColumn(names, "path");
	bind->cols.content = FindColumn(names, "content");
	bind->cols.title = FindColumn(names, "title");
	bind->cols.mimetype = FindColumn(names, "mimetype");

	if (bind->cols.path < 0) {
		throw BinderException("COPY TO (FORMAT zim): the input must have a 'path' column");
	}
	if (bind->cols.content < 0) {
		throw BinderException("COPY TO (FORMAT zim): the input must have a 'content' column");
	}

	// Derive the mimetype default from the content column's SQL type. libzim writes
	// "WARNING: mimetype missing for <path>" to stderr once per row, so defaulting
	// (rather than passing NULL through) is what keeps a large archive's stderr usable.
	bind->default_mimetype = sql_types[static_cast<idx_t>(bind->cols.content)].id() == LogicalTypeId::BLOB
	                             ? "application/octet-stream"
	                             : "text/plain";
	return std::move(bind);
}

unique_ptr<LocalFunctionData> ZimCopyInitLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq<ZimCopyLocalState>();
}

unique_ptr<GlobalFunctionData> ZimCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                 const string &file_path) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();
	auto state = make_uniq<ZimCopyGlobalState>(context, file_path);
	state->writer = make_uniq<ZimWriter>(file_path, bind.config);
	return std::move(state);
}

// Read a VARCHAR/BLOB cell as raw bytes; returns false when the cell is NULL.
bool GetStringCell(DataChunk &chunk, int64_t col, idx_t row, string &out) {
	if (col < 0) {
		return false;
	}
	auto &vec = chunk.data[static_cast<idx_t>(col)];
	if (!FlatVector::Validity(vec).RowIsValid(row)) {
		return false;
	}
	out = FlatVector::GetData<string_t>(vec)[row].GetString();
	return true;
}

void ZimCopySink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p,
                 LocalFunctionData &lstate, DataChunk &input) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();
	auto &gstate = gstate_p.Cast<ZimCopyGlobalState>();
	input.Flatten();

	for (idx_t row = 0; row < input.size(); row++) {
		ZimWriteEntry entry;
		if (!GetStringCell(input, bind.cols.path, row, entry.path)) {
			throw InvalidInputException("COPY TO (FORMAT zim): 'path' must not be NULL");
		}
		if (!GetStringCell(input, bind.cols.title, row, entry.title)) {
			entry.title = entry.path;
		}
		if (!GetStringCell(input, bind.cols.mimetype, row, entry.mimetype)) {
			entry.mimetype = bind.default_mimetype;
		}
		GetStringCell(input, bind.cols.content, row, entry.content);
		gstate.seen_paths.insert(entry.path);
		gstate.writer->AddItem(entry);
	}
}

void ZimCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                    LocalFunctionData &lstate) {
}

void ZimCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<ZimCopyGlobalState>();
	gstate.writer->Finish();
	gstate.writer.reset();
	gstate.finished = true; // suppresses the destructor's unlink
}

// A single Creator is not documented as safe for concurrent addItem(), and entry
// order carries no contract we need. Parallelism comes from libzim's own workers
// (configNbWorkers), not from DuckDB.
CopyFunctionExecutionMode ZimCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

} // namespace

void RegisterCopyToZim(ExtensionLoader &loader) {
	CopyFunction function("zim");
	function.copy_to_bind = ZimCopyBind;
	function.copy_to_initialize_local = ZimCopyInitLocal;
	function.copy_to_initialize_global = ZimCopyInitGlobal;
	function.copy_to_sink = ZimCopySink;
	function.copy_to_combine = ZimCopyCombine;
	function.copy_to_finalize = ZimCopyFinalize;
	function.execution_mode = ZimCopyExecutionMode;
	function.extension = "zim";
	loader.RegisterFunction(function);
}

} // namespace duckdb
```

- [ ] **Step 6: Wire the build and registration**

In `CMakeLists.txt`, add the two sources to `EXTENSION_SOURCES` (after `src/zim_search.cpp`
on line 32):

```cmake
    src/zim_search.cpp
    src/zim_writer.cpp
    src/copy_to_zim.cpp)
```

In `src/zim_extension.cpp`, add the forward declaration beside the others (~line 23):

```cpp
void RegisterZimSearch(ExtensionLoader &loader);     // phase 3 (xapian FTS)
void RegisterCopyToZim(ExtensionLoader &loader);     // phase 4 (COPY TO)
```

and the call at the end of `LoadInternal` (~line 53):

```cpp
	RegisterZimSearch(loader);
	RegisterCopyToZim(loader);
```

- [ ] **Step 7: Build and run the test**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim.test"
```

Expected: PASS. If `zim::Compression::None` does not compile, check the enumerator spelling
in `build/release/vcpkg_installed/x64-linux/include/zim/zim.h` and fix `ParseCompression`;
libzim spells these `Zstd` / `Lzma` / `None` in 9.7.0 but verify rather than assume.

- [ ] **Step 8: Run the full suite to confirm nothing regressed**

```bash
./build/release/test/unittest "test/sql/*"
```

Expected: all previously-passing tests still pass (326 assertions before this task, plus the
new ones).

- [ ] **Step 9: Commit**

```bash
make format
git add src/zim_writer.hpp src/zim_writer.cpp src/copy_to_zim.cpp \
        src/zim_extension.cpp CMakeLists.txt test/sql/copy_zim.test
git commit -m "feat(copy): COPY ... TO ... (FORMAT zim) writes items"
```

---

## Task 2: Refuse to overwrite, and unlink on error

**Files:**
- Modify: `src/copy_to_zim.cpp` (`ZimCopyInitGlobal`)
- Test: `test/sql/copy_zim_errors.test` (create)

**Interfaces:**
- Consumes: `ZimCopyGlobalState` from Task 1 (its destructor already unlinks; this task adds
  the existence check and proves both behaviours).
- Produces: no new symbols.

This is the most important task in the plan. Per design §7.2, a failed write leaves an
archive that opens, checksums, and passes `zim_check()` — so "leave the partial file" means
presenting a truncated corpus as a healthy one.

- [ ] **Step 1: Write the failing test**

Create `test/sql/copy_zim_errors.test`:

```
# name: test/sql/copy_zim_errors.test
# description: COPY ... TO ... (FORMAT zim) -- refusals and cleanup
# group: [sql]

require zim

statement ok
PRAGMA enable_verification

# --- the input must carry the required columns ------------------------------
statement error
COPY (SELECT 'x' AS notpath, 'y' AS content) TO '__TEST_DIR__/e1.zim' (FORMAT zim);
----
must have a 'path' column

statement error
COPY (SELECT 'x' AS path) TO '__TEST_DIR__/e2.zim' (FORMAT zim);
----
must have a 'content' column

# --- refusing to overwrite --------------------------------------------------
statement ok
COPY (SELECT 'A/One' AS path, 'first' AS content)
TO '__TEST_DIR__/nooverwrite.zim' (FORMAT zim);

statement error
COPY (SELECT 'A/Two' AS path, 'second' AS content)
TO '__TEST_DIR__/nooverwrite.zim' (FORMAT zim);
----
already exists

# the original survives untouched -- the refusal must not have damaged it
query II
SELECT path, content
FROM read_zim('__TEST_DIR__/nooverwrite.zim',
              include_content := true, content_as_varchar := true);
----
A/One	first

# --- a mid-stream error leaves NO output ------------------------------------
# The CAST fails partway through, after the sink has already taken chunks. The
# global state's destructor must unlink the partial archive.
statement error
COPY (SELECT 'A/' || i AS path,
             (CASE WHEN i < 5000 THEN '1' ELSE 'not-a-number' END)::INTEGER::VARCHAR AS content
      FROM range(10000) t(i))
TO '__TEST_DIR__/aborted.zim' (FORMAT zim);
----
Conversion Error

# If the file still existed this would succeed and return rows; it must fail to open.
statement error
SELECT count(*) FROM read_zim('__TEST_DIR__/aborted.zim');
----
failed to open ZIM

# and nothing was left at the output path
# NOTE: assert absence with glob(), not zim_check(). `zim_check` returns false for
# BOTH "file absent" and "file present but corrupt", so it cannot distinguish the
# two -- and on this branch it raises for an unopenable archive anyway (that fix
# lives in a sibling PR). A zero glob count means exactly "no file".
query I
SELECT count(*) FROM glob('__TEST_DIR__/aborted.zim');
----
0
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: FAIL at the overwrite case — the second `COPY` succeeds instead of erroring
(libzim happily truncates the existing file).

- [ ] **Step 3: Add the existence check**

In `src/copy_to_zim.cpp`, replace the body of `ZimCopyInitGlobal`:

```cpp
unique_ptr<GlobalFunctionData> ZimCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                 const string &file_path) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();

	// Deliberate deviation from parquet/csv, which clobber by default. A ZIM is
	// often the only copy of a corpus that took hours to build, and a failed write
	// leaves a valid-looking archive (§7.2) -- clobber-by-default plus
	// silent-partial-success destroys data and then reports health. Refusing also
	// subsumes the self-reference hazard: a source archive necessarily exists, so
	// it can never be a valid output path.
	auto &fs = FileSystem::GetFileSystem(context);
	if (fs.FileExists(file_path)) {
		throw InvalidInputException(
		    "COPY TO (FORMAT zim): output '%s' already exists; refusing to overwrite. "
		    "A ZIM is written once -- remove the file first if you mean to replace it.",
		    file_path);
	}

	auto state = make_uniq<ZimCopyGlobalState>(context, file_path);
	state->writer = make_uniq<ZimWriter>(file_path, bind.config);
	return std::move(state);
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: PASS. If the mid-stream case fails because the file still exists, the destructor
is not running — confirm `ZimCopyGlobalState` is destroyed on the error path by adding a
temporary `fprintf(stderr, ...)` in the destructor, then remove it.

- [ ] **Step 5: Verify the unlink guarantee is real, not accidental**

Temporarily comment out the `fs.RemoveFile(out_path)` line, rebuild, and re-run. The
"mid-stream error leaves NO output" cases must FAIL. Restore the line and confirm they pass
again. A cleanup test that passes with the cleanup removed is worthless.

- [ ] **Step 6: Commit**

```bash
make format
git add src/copy_to_zim.cpp test/sql/copy_zim_errors.test
git commit -m "feat(copy): refuse to overwrite, and unlink partial output on error"
```

---

## Task 3: Duplicate paths and `ON_CONFLICT`

**Files:**
- Modify: `src/copy_to_zim.cpp` (`ZimCopyBindData`, `ZimCopyBind`, `ZimCopySink`)
- Test: `test/sql/copy_zim_errors.test`

**Interfaces:**
- Consumes: `ZimCopyGlobalState::seen_paths` (Task 1).
- Produces: `ZimConflictPolicy` enum in the anonymous namespace of `copy_to_zim.cpp`.

libzim throws `InvalidEntry` mid-stream on a duplicate. We detect first so the error names
the SQL problem — and because `COPY` consumes arbitrary query output, a duplicate is one
careless `GROUP BY` away.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/copy_zim_errors.test`:

```
# --- duplicate paths --------------------------------------------------------
statement error
COPY (SELECT * FROM (VALUES ('A/Dup', 'first'), ('A/Dup', 'second')) t(path, content))
TO '__TEST_DIR__/dup.zim' (FORMAT zim);
----
duplicate path 'A/Dup'

# and it left nothing behind (glob, not zim_check -- see the note above)
query I
SELECT count(*) FROM glob('__TEST_DIR__/dup.zim');
----
0

# ON_CONFLICT 'first' keeps the first occurrence
statement ok
COPY (SELECT * FROM (VALUES ('A/Dup', 'first'), ('A/Dup', 'second')) t(path, content))
TO '__TEST_DIR__/dupfirst.zim' (FORMAT zim, ON_CONFLICT 'first');

query II
SELECT path, content
FROM read_zim('__TEST_DIR__/dupfirst.zim',
              include_content := true, content_as_varchar := true);
----
A/Dup	first

# 'last' cannot be done streaming and is rejected at bind time, not silently buffered
statement error
COPY (SELECT 'A/x' AS path, 'y' AS content)
TO '__TEST_DIR__/duplast.zim' (FORMAT zim, ON_CONFLICT 'last');
----
ON_CONFLICT 'last' would require buffering every row

statement error
COPY (SELECT 'A/x' AS path, 'y' AS content)
TO '__TEST_DIR__/dupbad.zim' (FORMAT zim, ON_CONFLICT 'sideways');
----
ON_CONFLICT must be 'error' or 'first'
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: FAIL — the first duplicate case surfaces libzim's own `Impossible to add …`
message rather than ours, and `ON_CONFLICT` is an unknown option.

- [ ] **Step 3: Add the policy and the check**

In `src/copy_to_zim.cpp`, add above `ZimCopyBindData`:

```cpp
enum class ZimConflictPolicy { ERROR_ON_DUPLICATE, KEEP_FIRST };
```

Add the field to `ZimCopyBindData` (and to its `Copy()`):

```cpp
	ZimConflictPolicy on_conflict = ZimConflictPolicy::ERROR_ON_DUPLICATE;
```

```cpp
		result->on_conflict = on_conflict;
```

In `ZimCopyBind`, before `return std::move(bind);`:

```cpp
	for (auto &option : input.info.options) {
		auto key = StringUtil::Lower(option.first);
		if (option.second.size() != 1) {
			throw BinderException("COPY TO (FORMAT zim): option '%s' takes exactly one value", option.first);
		}
		auto &value = option.second[0];
		if (key == "on_conflict") {
			auto policy = StringUtil::Lower(value.ToString());
			if (policy == "error") {
				bind->on_conflict = ZimConflictPolicy::ERROR_ON_DUPLICATE;
			} else if (policy == "first") {
				bind->on_conflict = ZimConflictPolicy::KEEP_FIRST;
			} else if (policy == "last") {
				throw BinderException(
				    "COPY TO (FORMAT zim): ON_CONFLICT 'last' would require buffering every row before "
				    "writing any, because a later duplicate can only win if nothing has been written yet. "
				    "Deduplicate in SQL instead, or use 'first'.");
			} else {
				throw BinderException("COPY TO (FORMAT zim): ON_CONFLICT must be 'error' or 'first'");
			}
		} else {
			throw BinderException("COPY TO (FORMAT zim): unknown option '%s'", option.first);
		}
	}
```

In `ZimCopySink`, replace the `seen_paths.insert` / `AddItem` pair:

```cpp
		if (!gstate.seen_paths.insert(entry.path).second) {
			if (bind.on_conflict == ZimConflictPolicy::KEEP_FIRST) {
				continue;
			}
			throw InvalidInputException(
			    "COPY TO (FORMAT zim): duplicate path '%s'. Entry paths must be unique within an "
			    "archive; deduplicate in SQL, or pass ON_CONFLICT 'first'.",
			    entry.path);
		}
		gstate.writer->AddItem(entry);
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
make format
git add src/copy_to_zim.cpp test/sql/copy_zim_errors.test
git commit -m "feat(copy): detect duplicate paths in the sink; add ON_CONFLICT"
```

---

## Task 4: Metadata, illustration and `MAIN_PATH`

**Files:**
- Modify: `src/copy_to_zim.cpp` (option loop, `ZimCopyFinalize`)
- Test: `test/sql/copy_zim.test`, `test/sql/copy_zim_errors.test`

**Interfaces:**
- Consumes: `ZimWriterConfig::metadata`, `::illustration`, `::main_path` (Task 1).
- Produces: no new symbols.

Per design §7.3, libzim accepts a `setMainPath()` pointing at a path that was never added,
silently producing an archive with no landing page. We validate against the paths actually
written.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/copy_zim.test`:

```
# --- metadata options -------------------------------------------------------
statement ok
COPY (SELECT 'A/Home' AS path, 'hi' AS content)
TO '__TEST_DIR__/meta.zim'
   (FORMAT zim, TITLE 'My Archive', DESCRIPTION 'a test', LANGUAGE 'eng',
    CREATOR 'me', PUBLISHER 'us', NAME 'testarchive', DATE '2026-08-17',
    TAGS 'test;demo', MAIN_PATH 'A/Home');

query I
SELECT zim_metadata('__TEST_DIR__/meta.zim', 'Title');
----
My Archive

query I
SELECT zim_metadata('__TEST_DIR__/meta.zim', 'Language');
----
eng

query I
SELECT zim_main_entry('__TEST_DIR__/meta.zim');
----
A/Home

# arbitrary keys via the METADATA map
statement ok
COPY (SELECT 'A/Home' AS path, 'hi' AS content)
TO '__TEST_DIR__/meta2.zim'
   (FORMAT zim, METADATA MAP {'Title': 'From Map', 'Source': 'nowhere'});

query I
SELECT zim_metadata('__TEST_DIR__/meta2.zim', 'Source');
----
nowhere
```

Append to `test/sql/copy_zim_errors.test`:

```
# --- MAIN_PATH must name an entry the query actually produced ---------------
statement error
COPY (SELECT 'A/Home' AS path, 'hi' AS content)
TO '__TEST_DIR__/badmain.zim' (FORMAT zim, MAIN_PATH 'A/Missing');
----
MAIN_PATH 'A/Missing' does not match any entry

query I
SELECT count(*) FROM glob('__TEST_DIR__/badmain.zim');
----
0

# a metadata key given twice, once named and once in the map, is an error
statement error
COPY (SELECT 'A/Home' AS path, 'hi' AS content)
TO '__TEST_DIR__/dupmeta.zim'
   (FORMAT zim, TITLE 'One', METADATA MAP {'Title': 'Two'});
----
metadata key 'Title' given twice
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim.test" && \
./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: FAIL — `unknown option 'title'`.

- [ ] **Step 3: Parse the metadata options**

In `src/copy_to_zim.cpp`, add above `ZimCopyBind`:

```cpp
// Named metadata options are sugar for METADATA. Each maps to its ZIM key.
const std::map<string, string> METADATA_OPTIONS = {
    {"title", "Title"},   {"description", "Description"}, {"language", "Language"},
    {"creator", "Creator"}, {"publisher", "Publisher"},   {"name", "Name"},
    {"date", "Date"},     {"tags", "Tags"}};

void SetMetadata(ZimWriterConfig &config, const string &key, const string &value) {
	if (!config.metadata.insert({key, value}).second) {
		throw BinderException(
		    "COPY TO (FORMAT zim): metadata key '%s' given twice; specify it either as a named option "
		    "or in METADATA, not both",
		    key);
	}
}
```

In the option loop in `ZimCopyBind`, before the final `else`:

```cpp
		} else if (METADATA_OPTIONS.count(key)) {
			SetMetadata(bind->config, METADATA_OPTIONS.at(key), value.ToString());
		} else if (key == "metadata") {
			if (value.type().id() != LogicalTypeId::MAP) {
				throw BinderException("COPY TO (FORMAT zim): METADATA must be a MAP(VARCHAR, VARCHAR)");
			}
			auto &entries = MapValue::GetChildren(value);
			for (auto &entry : entries) {
				auto &kv = StructValue::GetChildren(entry);
				SetMetadata(bind->config, kv[0].ToString(), kv[1].ToString());
			}
		} else if (key == "illustration") {
			bind->config.illustration = value.ToString();
		} else if (key == "main_path") {
			bind->config.main_path = value.ToString();
```

- [ ] **Step 4: Validate `MAIN_PATH` in finalize**

Replace `ZimCopyFinalize`:

```cpp
void ZimCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();
	auto &gstate = gstate_p.Cast<ZimCopyGlobalState>();

	// libzim accepts a main path that was never added and silently produces an
	// archive with has_main_entry = false (§7.3). Validate against what we wrote.
	if (!bind.config.main_path.empty() && !gstate.seen_paths.count(bind.config.main_path)) {
		throw InvalidInputException(
		    "COPY TO (FORMAT zim): MAIN_PATH '%s' does not match any entry in the input",
		    bind.config.main_path);
	}
	gstate.writer->Finish();
	gstate.writer.reset();
	gstate.finished = true;
}
```

- [ ] **Step 5: Run both tests to verify they pass**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: PASS. If `MapValue::GetChildren` / `StructValue::GetChildren` do not compile,
check `duckdb/src/include/duckdb/common/types/value.hpp` for the current spelling — DuckDB
has renamed these helpers across versions.

- [ ] **Step 6: Commit**

```bash
make format
git add src/copy_to_zim.cpp test/sql/copy_zim.test test/sql/copy_zim_errors.test
git commit -m "feat(copy): metadata options, illustration, validated MAIN_PATH"
```

---

## Task 5: Fulltext index and creator tuning

**Files:**
- Modify: `src/copy_to_zim.cpp` (option loop)
- Test: `test/sql/copy_zim.test`, `test/sql/copy_zim_errors.test`

**Interfaces:**
- Consumes: `ZimWriterConfig::index`, `::index_language`, `::compression`, `::cluster_size`,
  `::workers` (Task 1).
- Produces: no new symbols.

Indexing is **off unless requested** (design §3.1). A requested index with no language is an
error, not a silent no-op — an unsearchable archive that was asked to be searchable is the
silent-failure shape this design keeps guarding against.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/copy_zim.test`:

```
# --- fulltext index ---------------------------------------------------------

statement ok
COPY (SELECT * FROM (VALUES ('A/Photo', 'Photosynthesis', 'text/html',
                             '<html><body>Photosynthesis converts light into sugar.</body></html>'),
                            ('A/Water', 'Water', 'text/html',
                             '<html><body>Water is essential to plants.</body></html>'))
        t(path, title, mimetype, content))
TO '__TEST_DIR__/indexed.zim' (FORMAT zim, LANGUAGE 'eng', INDEX true);

query I
SELECT count(*) > 0 FROM zim_search('__TEST_DIR__/indexed.zim', 'photosynthesis');
----
true

# without INDEX, no fulltext index is built
statement ok
COPY (SELECT 'A/Photo' AS path, 'text/html' AS mimetype,
             '<html><body>Photosynthesis</body></html>' AS content)
TO '__TEST_DIR__/unindexed.zim' (FORMAT zim, LANGUAGE 'eng');

query I
SELECT (zim_info('__TEST_DIR__/unindexed.zim')).has_fulltext_index;
----
false
```

Append to `test/sql/copy_zim_errors.test`:

```
# --- INDEX needs a language -------------------------------------------------
statement error
COPY (SELECT 'A/x' AS path, 'y' AS content)
TO '__TEST_DIR__/noidxlang.zim' (FORMAT zim, INDEX true);
----
INDEX requires a language

statement error
COPY (SELECT 'A/x' AS path, 'y' AS content)
TO '__TEST_DIR__/badcomp.zim' (FORMAT zim, COMPRESSION 'gzip');
----
COMPRESSION must be
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: FAIL — `unknown option 'index'`.

- [ ] **Step 3: Parse the options**

Add to the option loop in `ZimCopyBind`:

```cpp
		} else if (key == "index") {
			bind->config.index = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (key == "index_language") {
			bind->config.index_language = value.ToString();
		} else if (key == "compression") {
			auto comp = StringUtil::Lower(value.ToString());
			// libzim 9.7.0 removed LZMA: zim/zim.h declares only { None = 1, Zstd = 5 },
			// with a comment that the intermediate values are no longer supported. Reject
			// 'lzma' by name rather than silently substituting zstd -- a caller who asked
			// for a specific compression and got a different one has been lied to.
			if (comp == "lzma") {
				throw BinderException(
				    "COPY TO (FORMAT zim): COMPRESSION 'lzma' is not available -- libzim 9.7.0 removed "
				    "LZMA support from the ZIM format. Use 'zstd' (the default) or 'none'.");
			}
			if (comp != "zstd" && comp != "none") {
				throw BinderException("COPY TO (FORMAT zim): COMPRESSION must be 'zstd' or 'none'");
			}
			bind->config.compression = comp;
		} else if (key == "cluster_size") {
			bind->config.cluster_size = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		} else if (key == "workers") {
			auto n = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
			if (n == 0) {
				throw BinderException("COPY TO (FORMAT zim): WORKERS must be at least 1");
			}
			bind->config.workers = static_cast<uint32_t>(n);
```

After the option loop, resolve the index language and validate:

```cpp
	if (bind->config.index && bind->config.index_language.empty()) {
		// Default the index language from LANGUAGE metadata when it was given.
		auto lang = bind->config.metadata.find("Language");
		if (lang != bind->config.metadata.end()) {
			bind->config.index_language = lang->second;
		}
	}
	if (bind->config.index && bind->config.index_language.empty()) {
		throw BinderException(
		    "COPY TO (FORMAT zim): INDEX requires a language for stemming. Pass LANGUAGE 'eng' "
		    "(which also sets the Language metadata) or INDEX_LANGUAGE 'eng'.");
	}
```

- [ ] **Step 4: Guard the WASM build**

Xapian is absent on emscripten (`vcpkg.json` drops the feature there), so a requested index
must fail loudly rather than silently produce an unsearchable archive.

The macro that decides this is **`LIBZIM_WITH_XAPIAN`**, which comes from
`zim/zim_config.h` — a *libzim* header. It is therefore only visible in `zim_writer.cpp`,
which is the only translation unit allowed to include libzim (see Global Constraints). The
existing reader uses the same macro at `src/zim_access.cpp:25`, `:448`, `:522`.

So expose it as a plain predicate. Add to `src/zim_writer.hpp`, after the `ZimWriter` class:

```cpp
// True when libzim was built with Xapian, i.e. this build can write a fulltext
// index. False on WebAssembly, which is search-less. Lets the DuckDB binding
// layer reject INDEX at bind time without including a libzim header.
bool ZimWriterHasFulltextIndexing();
```

Add to `src/zim_writer.cpp`, inside `namespace zim_ext`:

```cpp
bool ZimWriterHasFulltextIndexing() {
#ifdef LIBZIM_WITH_XAPIAN
	return true;
#else
	return false;
#endif
}
```

Add `using zim_ext::ZimWriterHasFulltextIndexing;` beside the other `using` declarations in
`copy_to_zim.cpp`, then add the check immediately after the index-language validation:

```cpp
	if (bind->config.index && !ZimWriterHasFulltextIndexing()) {
		throw BinderException(
		    "COPY TO (FORMAT zim): INDEX true was requested, but this build has no Xapian support "
		    "(WebAssembly builds are search-less), so no fulltext index can be written. Omit INDEX "
		    "to write an archive without one.");
	}
```

- [ ] **Step 5: Run the tests**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
make format
git add src/zim_writer.hpp src/zim_writer.cpp src/copy_to_zim.cpp \
        test/sql/copy_zim.test test/sql/copy_zim_errors.test
git commit -m "feat(copy): INDEX/INDEX_LANGUAGE, COMPRESSION, CLUSTER_SIZE, WORKERS"
```

---

## Task 6: Writer tolerance — make the round trip bind

**Files:**
- Modify: `src/copy_to_zim.cpp` (`ZimColumns`, `ZimCopyBind`, `ZimCopySink`)
- Test: `test/sql/copy_zim.test`, `test/sql/copy_zim_errors.test`

**Interfaces:**
- Consumes: `ZimColumns` (Task 1), `ZimWriteEntry::is_redirect` / `::redirect_path`
  (Task 1), `ZimWriter::AddItem` (which already routes redirects to `addRedirection`).
- Produces: no new symbols.

Per design §6.1: `COPY (FROM read_zim(x)) TO y` must bind and succeed. `read_zim` emits
`is_redirect`, `redirect_path`, `size` and `file_path`, none of which the writer knows yet.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/copy_zim.test`:

```
# --- the naive round trip binds and succeeds --------------------------------
statement ok
COPY (SELECT * FROM read_zim('test/oracle/test.zim',
                             include_content := true, content_as_varchar := true))
TO '__TEST_DIR__/roundtrip.zim' (FORMAT zim);

# same entry count, and the redirect survived as a redirect
query I
SELECT (SELECT count(*) FROM read_zim('__TEST_DIR__/roundtrip.zim'))
     = (SELECT count(*) FROM read_zim('test/oracle/test.zim'));
----
true

query II
SELECT is_redirect, redirect_path
FROM read_zim('__TEST_DIR__/roundtrip.zim')
WHERE is_redirect
ORDER BY path;
----
true	A/Calcium

# content matches entry for entry
query I
SELECT count(*)
FROM read_zim('test/oracle/test.zim', include_content := true, content_as_varchar := true) a
JOIN read_zim('__TEST_DIR__/roundtrip.zim', include_content := true, content_as_varchar := true) b
  USING (path)
WHERE a.content IS DISTINCT FROM b.content;
----
0

# the archive-level counters agree too -- a content-only assertion cannot fail
# where the round trip is not an identity (design §8)
query I
SELECT (SELECT (zim_info('__TEST_DIR__/roundtrip.zim')).entry_count)
     = (SELECT (zim_info('test/oracle/test.zim')).entry_count);
----
true
```

> If `test/oracle/test.zim`'s redirect target differs from `A/Calcium`, run
> `SELECT path, redirect_path FROM read_zim('test/oracle/test.zim') WHERE is_redirect;`
> first and use the real value. Do not guess.

Append to `test/sql/copy_zim_errors.test`:

```
# --- a genuinely unknown column is still an error ---------------------------
statement error
COPY (SELECT 'A/x' AS path, 'y' AS content, 'z' AS titel)
TO '__TEST_DIR__/typo.zim' (FORMAT zim);
----
unknown column 'titel'

# --- v2 columns are refused as deferred, not as typos -----------------------
# entry_kind is a v2 column. In v1 it cannot be accepted at all, so the error
# says "not supported yet" rather than "unknown column" -- a deferral must read
# as a deferral. The mutual-exclusion check against is_redirect belongs to v2,
# when entry_kind becomes a column that CAN be supplied.
statement error
COPY (SELECT 'A/x' AS path, 'y' AS content, false AS is_redirect, 'item' AS entry_kind)
TO '__TEST_DIR__/bothspellings.zim' (FORMAT zim);
----
not supported yet
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: FAIL — the round trip currently succeeds silently *ignoring* `is_redirect` (so the
redirect assertion fails), and the typo case does not error at all.

- [ ] **Step 3: Extend column resolution**

In `src/copy_to_zim.cpp`, extend `ZimColumns`:

```cpp
struct ZimColumns {
	int64_t path = -1;
	int64_t content = -1;
	int64_t title = -1;
	int64_t mimetype = -1;
	int64_t is_redirect = -1;
	int64_t redirect_path = -1;
	int64_t front_article = -1;
	int64_t compress = -1;
};
```

Replace the column-resolution block in `ZimCopyBind`:

```cpp
	bind->cols.path = FindColumn(names, "path");
	bind->cols.content = FindColumn(names, "content");
	bind->cols.title = FindColumn(names, "title");
	bind->cols.mimetype = FindColumn(names, "mimetype");
	bind->cols.is_redirect = FindColumn(names, "is_redirect");
	bind->cols.redirect_path = FindColumn(names, "redirect_path");
	bind->cols.front_article = FindColumn(names, "front_article");
	bind->cols.compress = FindColumn(names, "compress");

	// Columns read_zim emits that carry no meaning for the writer. Accepting them
	// is what makes COPY (FROM read_zim(x)) TO y work (design §6.1); `size` is
	// derived by libzim and `file_path` is provenance.
	static const char *IGNORED_COLUMNS[] = {"size", "file_path"};
	// v2 columns: named so the deferral reads as a deferral, not as a typo.
	static const char *DEFERRED_COLUMNS[] = {"content_path", "entry_kind", "target",
	                                         "source_archive", "source_entry"};

	for (idx_t i = 0; i < names.size(); i++) {
		auto &n = names[i];
		bool known = false;
		for (auto *k : {"path", "content", "title", "mimetype", "is_redirect", "redirect_path",
		                "front_article", "compress"}) {
			known |= StringUtil::CIEquals(n, k);
		}
		for (auto *k : IGNORED_COLUMNS) {
			known |= StringUtil::CIEquals(n, k);
		}
		for (auto *k : DEFERRED_COLUMNS) {
			if (StringUtil::CIEquals(n, k)) {
				throw BinderException(
				    "COPY TO (FORMAT zim): column '%s' is not supported yet (planned for the next "
				    "version, which adds content locators, aliases and redirect kinds)",
				    n);
			}
		}
		if (!known) {
			throw BinderException("COPY TO (FORMAT zim): unknown column '%s'", n);
		}
	}

	if (bind->cols.path < 0) {
		throw BinderException("COPY TO (FORMAT zim): the input must have a 'path' column");
	}
	if (bind->cols.content < 0) {
		throw BinderException("COPY TO (FORMAT zim): the input must have a 'content' column");
	}
```

> `entry_kind` is in `DEFERRED_COLUMNS`, so the "v2 columns are refused as deferred" test
> above is satisfied by that branch alone. Do **not** add a mutual-exclusion check between
> `is_redirect` and `entry_kind` — a column that cannot be accepted at all cannot conflict
> with anything. That check belongs to v2, when `entry_kind` becomes real.

- [ ] **Step 4: Use the redirect columns in the sink**

In `ZimCopySink`, after the mimetype default and before `AddItem`:

```cpp
		if (bind.cols.is_redirect >= 0) {
			auto &vec = input.data[static_cast<idx_t>(bind.cols.is_redirect)];
			if (FlatVector::Validity(vec).RowIsValid(row) && FlatVector::GetData<bool>(vec)[row]) {
				entry.is_redirect = true;
				if (!GetStringCell(input, bind.cols.redirect_path, row, entry.redirect_path)) {
					throw InvalidInputException(
					    "COPY TO (FORMAT zim): entry '%s' has is_redirect = true but no redirect_path",
					    entry.path);
				}
			}
		}
		if (bind.cols.front_article >= 0) {
			auto &vec = input.data[static_cast<idx_t>(bind.cols.front_article)];
			if (FlatVector::Validity(vec).RowIsValid(row)) {
				entry.has_front_article = true;
				entry.front_article = FlatVector::GetData<bool>(vec)[row];
			}
		}
		if (bind.cols.compress >= 0) {
			auto &vec = input.data[static_cast<idx_t>(bind.cols.compress)];
			if (FlatVector::Validity(vec).RowIsValid(row)) {
				entry.has_compress = true;
				entry.compress = FlatVector::GetData<bool>(vec)[row];
			}
		}
```

A redirect has no mimetype, so skip the `text/plain` default for it — move the mimetype
default to after the redirect check and guard it:

```cpp
		if (!entry.is_redirect && entry.mimetype.empty()) {
			entry.mimetype = "text/plain";
		}
```

- [ ] **Step 5: Run the tests**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: PASS. If the round trip's `entry_count` differs, check whether the source archive
contains an alias — design §8 explains why an alias becomes a duplicated item and the
counters diverge. If so, record the actual delta in the test with a comment rather than
asserting equality.

- [ ] **Step 6: Run the full suite**

```bash
./build/release/test/unittest "test/sql/*"
```

- [ ] **Step 7: Commit**

```bash
make format
git add src/copy_to_zim.cpp test/sql/copy_zim.test test/sql/copy_zim_errors.test
git commit -m "feat(copy): accept read_zim's native columns so the round trip binds"
```

---

## Task 7: Reject unsupported copy options explicitly

**Files:**
- Modify: `src/copy_to_zim.cpp` (`ZimCopyBind`, register `rotate_files`)
- Test: `test/sql/copy_zim_errors.test`

**Interfaces:**
- Consumes: the option loop from Task 3.
- Produces: `ZimCopyRotateFiles` in the anonymous namespace.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/copy_zim_errors.test`:

```
# --- options that must be refused, not ignored ------------------------------
statement error
COPY (SELECT 'A/x' AS path, 'y' AS content, 'en' AS lang)
TO '__TEST_DIR__/parts' (FORMAT zim, PARTITION_BY (lang));
----
PARTITION_BY is not supported yet

statement error
COPY (SELECT 'A/x' AS path, 'y' AS content)
TO '__TEST_DIR__/rot.zim' (FORMAT zim, FILE_SIZE_BYTES 1000);
----
FILE_SIZE_BYTES
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build/release/test/unittest "test/sql/copy_zim_errors.test"
```

Expected: FAIL — `PARTITION_BY` is handled by DuckDB before our bind sees it, so the first
case likely succeeds and writes a directory of archives.

> **Read this before writing code — the controller verified it against the pinned DuckDB,
> and an earlier draft of this task was wrong.**
>
> `bind_copy.cpp:122-123` does `stmt.info->options.clear()` and then re-adds only the
> options it does **not** recognise. Every DuckDB-level option — `partition_by`,
> `file_size_bytes`, `use_tmp_file`, `overwrite`, `filename_pattern`, `file_extension`,
> `per_thread_output` — is consumed there and **never reaches `copy_to_bind`**. So an
> option-loop branch for any of them is dead code. Do not write one.

- [ ] **Step 3: Reject `FILE_SIZE_BYTES` by leaving `rotate_files` unset**

DuckDB already does this for us, and does it better. `bind_copy.cpp:166-168`:

```cpp
		} else if (loption == "file_size_bytes") {
			…
			if (!function.rotate_files) {
				throw NotImplementedException("FILE_SIZE_BYTES not implemented for FORMAT \"%s\"", …);
			}
```

So the correct action is to **do nothing**: leave `function.rotate_files` unset (it is
`nullptr` by default) and DuckDB raises a clear, format-named error.

Setting it to a function returning `false` — which an earlier draft of this plan
instructed — would be actively harmful: it makes `!function.rotate_files` false, so
DuckDB *skips* its own rejection and proceeds into rotation logic that then refuses to
rotate. Do not add `ZimCopyRotateFiles`. Confirm no `rotate_files` assignment exists in
`RegisterCopyToZim`.

- [ ] **Step 4: Reject `PARTITION_BY` via `copy_to_initialize_operator`**

Because the option never reaches our bind, the rejection needs a hook that can see the
physical operator. `copy_to_initialize_operator` receives the `PhysicalOperator`, which for
this path is a `PhysicalCopyToFile` carrying `partition_columns`.

Add the include at the top of `src/copy_to_zim.cpp`:

```cpp
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
```

Add the hook beside the other functions in the anonymous namespace:

```cpp
// PARTITION_BY is consumed by DuckDB's binder and never reaches copy_to_bind, so the
// rejection has to happen where the physical operator is visible. Writing one archive per
// key is planned, but a partition can fragment into several archives past
// partitioned_write_max_open_files, and MAIN_PATH cannot survive partitioning -- neither is
// handled in this version, so refuse rather than produce a surprising result.
void ZimCopyInitializeOperator(GlobalFunctionData &gstate, const PhysicalOperator &op) {
	auto &copy_op = op.Cast<PhysicalCopyToFile>();
	if (!copy_op.partition_columns.empty()) {
		throw NotImplementedException(
		    "COPY TO (FORMAT zim): PARTITION_BY is not supported yet. Writing one archive per key is "
		    "planned, but a partition can fragment into several archives and MAIN_PATH cannot survive "
		    "partitioning, so it needs handling this version does not have.");
	}
}
```

and in `RegisterCopyToZim`:

```cpp
	function.initialize_operator = ZimCopyInitializeOperator;
```

Note this fires *after* `copy_to_initialize_global`, so the output file already exists when
it throws — the global state's destructor (Task 2) unlinks it. Confirm that with the test:
after the `PARTITION_BY` failure, the output directory must not contain a `.zim`.

If `PhysicalCopyToFile` turns out not to be includable from an extension (a link error on
`partition_columns`, or the header not being installed), fall back to detecting a hive
segment in the path passed to `copy_to_initialize_global`: a path containing `=` in its
last directory component means partitioned output. Record in a comment which mechanism you
used and why, because the next person will wonder.

- [ ] **Step 5: Run the tests**

```bash
make release && ./build/release/test/unittest "test/sql/copy_zim*.test"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
make format
git add src/copy_to_zim.cpp test/sql/copy_zim_errors.test
git commit -m "feat(copy): reject PARTITION_BY and FILE_SIZE_BYTES explicitly"
```

---

## Task 8: Stop libzim's writer from writing to stdout

**Files:**
- Create: `vcpkg_ports/libzim/no-writer-stdout.patch`, `test/no_writer_stdout_pollution.sh`
- Modify: `vcpkg_ports/libzim/portfile.cmake`, `vcpkg_ports/libzim/vcpkg.json`,
  `.github/workflows/MainDistributionPipeline.yml:98`

**Interfaces:**
- Consumes: the working `COPY` from Tasks 1–7.
- Produces: no new symbols.

In 9.7.0, `INFO()` in `src/writer/creator.cpp` is an **ungated** `std::cout` — `TINFO` and
`TPROGRESS` check `m_verbose`, `INFO` does not. Four of its six call sites fire on a normal
write. In a DuckDB CLI this is output corruption; in an MCP stdio server sharing stdout with
JSON-RPC it is protocol corruption. This mirrors `no-stemming-stdout.patch` (issue #21).

- [ ] **Step 1: Write the failing test**

Create `test/no_writer_stdout_pollution.sh` (model: `test/no_stdout_pollution.sh`):

```bash
#!/usr/bin/env bash
# Regression test: writing a ZIM must not print anything to stdout but the query
# result.
#
# libzim's writer defines INFO(e) as an UNGATED `std::cout << e`, unlike TINFO /
# TPROGRESS which check m_verbose. configVerbose(false) therefore cannot silence
# it. Our overlay patch (vcpkg_ports/libzim/no-writer-stdout.patch) drops the
# prints. This asserts they stay gone -- something sqllogictest can't check,
# because it compares query results via the API and never inspects stdout.
#
# Env overrides match test/no_stdout_pollution.sh: DUCKDB_BIN, ZIM_EXTENSION.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$repo/build/release/duckdb}"
ZIM_EXTENSION="${ZIM_EXTENSION:-$repo/build/release/extension/zim/zim.duckdb_extension}"

[ -x "$DUCKDB_BIN" ] || { echo "SKIP: duckdb shell not found at $DUCKDB_BIN (build first)"; exit 0; }
[ -f "$ZIM_EXTENSION" ] || { echo "SKIP: zim extension not found at $ZIM_EXTENSION (build first)"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
out="$tmp/written.zim"

# A write that exercises the indexing and redirect-resolution paths, since those
# are where the INFO() call sites live. The redirect below is DANGLING on
# purpose: libzim removes invalid redirections at finishZimCreation() and
# announces each removal through the very prints we are suppressing, so a patch
# tested only against a well-formed archive would never exercise those two sites.
stdout="$("$DUCKDB_BIN" -unsigned -noheader -list -c "
LOAD '$ZIM_EXTENSION';
COPY (SELECT * FROM (VALUES
        ('A/One', 'One', 'text/html', '<html><body>alpha</body></html>', false, NULL),
        ('A/Two', 'Two', 'text/html', '<html><body>beta</body></html>',  false, NULL),
        ('A/Gone','Gone','text/html', NULL, true, 'A/DoesNotExist'))
      t(path, title, mimetype, content, is_redirect, redirect_path))
TO '$out' (FORMAT zim, LANGUAGE 'eng', INDEX true);
SELECT 'WROTE_OK';" 2>/dev/null)"

echo "--- stdout of COPY ... TO ... (FORMAT zim) ---"
echo "$stdout"

fail=0

# 1) The write actually ran, so the assertion is meaningful.
if ! grep -q "WROTE_OK" <<<"$stdout"; then
  echo "FAIL: the COPY did not complete; assertion would be vacuous"; fail=1
fi

# 2) The regression guard: none of libzim's INFO() strings leaked to stdout.
for s in "Set entry indices" "Index titles" "Detect dangling redirects" \
         "Detect loops and/or blind chains of redirects" \
         "Removing invalid redirection" "Redirection "; do
  if grep -qF "$s" <<<"$stdout"; then
    echo "FAIL: libzim leaked '$s' to stdout"; fail=1
  fi
done

if [ "$fail" -eq 0 ]; then echo "PASS: COPY TO zim works and stdout is clean"; fi
exit "$fail"
```

Make it executable: `chmod +x test/no_writer_stdout_pollution.sh`

- [ ] **Step 2: Run it to verify it fails**

```bash
bash test/no_writer_stdout_pollution.sh
```

Expected: FAIL, listing several leaked strings.

- [ ] **Step 3: Write the patch**

Create `vcpkg_ports/libzim/no-writer-stdout.patch`. Base it on the **actual** file — read
`~/vcpkg/buildtrees/libzim/src/9.7.0-*/src/writer/creator.cpp` around the macro definition
(circa line 72) and produce a unified diff that changes only the `INFO` macro:

```diff
diff --git a/src/writer/creator.cpp b/src/writer/creator.cpp
index xxxxxxx..yyyyyyy 100644
--- a/src/writer/creator.cpp
+++ b/src/writer/creator.cpp
@@ -70,10 +70,12 @@ log_define("zim.writer.creator")
 
+// Local overlay: INFO() used to write unconditionally to std::cout, unlike
+// TINFO/TPROGRESS which are gated on m_verbose. That pollutes the caller's
+// stdout on every archive write. Keep the log_info() call (which honours
+// libzim's own logging configuration) and drop the print.
 #define INFO(e) \
     do { \
         log_info(e); \
-        std::cout << e << std::endl; \
     } while(false)
```

Verify the hunk applies before wiring it up:

```bash
cd ~/vcpkg/buildtrees/libzim/src/9.7.0-*/ && \
  git apply --check /home/teague/Projects/duckdb_zim/vcpkg_ports/libzim/no-writer-stdout.patch \
  && echo "PATCH APPLIES"
```

If the source tree is not a git checkout, use `patch -p1 --dry-run < …` instead.

- [ ] **Step 4: Wire the patch into the port**

In `vcpkg_ports/libzim/portfile.cmake`, add after `no-stemming-stdout.patch`:

```cmake
        # Local overlay only: libzim's writer defines INFO(e) as an UNGATED
        # `std::cout << e`, unlike TINFO/TPROGRESS which check m_verbose. Four of its
        # six call sites fire on a normal write ("Set entry indices", "Index titles",
        # "Detect dangling redirects", "Detect loops and/or blind chains of
        # redirects"); the other two announce removed invalid redirections. That
        # pollutes the caller's stdout on every COPY TO. Keep log_info(), drop the
        # print. Propose upstream; drop this overlay once libzim stops printing.
        no-writer-stdout.patch
```

In `vcpkg_ports/libzim/vcpkg.json`, bump `"port-version": 2` to `3` so vcpkg rebuilds.

- [ ] **Step 5: Rebuild and re-run**

```bash
make release && bash test/no_writer_stdout_pollution.sh
```

Expected: `PASS: COPY TO zim works and stdout is clean`.

This step rebuilds libzim from source and is slow (many minutes). That is expected.

- [ ] **Step 6: Record what was lost**

Suppressing the two redirection-removal prints removes the **only** signal that libzim
dropped entries. Add a `TODO` comment in `src/zim_writer.cpp` above `Finish()`:

```cpp
	// NOTE: libzim silently removes dangling redirects at finishZimCreation(). Before
	// our overlay patch it announced each removal on stdout; that print is now gone, so
	// the information is currently lost. When redirects become writable (v2), surface
	// removals as a DuckDB warning or a rejected-row count -- dropping entries silently
	// is worse than the pollution was. See docs/dev/copy-to-zim-design.md §7.4.
```

- [ ] **Step 7: Wire into CI**

In `.github/workflows/MainDistributionPipeline.yml`, beside the existing invocation on
line 98:

```yaml
          DUCKDB_BIN="$PWD/duckdb-cli/duckdb" ZIM_EXTENSION="$ext" bash test/no_writer_stdout_pollution.sh
```

- [ ] **Step 8: Commit**

```bash
git add vcpkg_ports/libzim/no-writer-stdout.patch vcpkg_ports/libzim/portfile.cmake \
        vcpkg_ports/libzim/vcpkg.json test/no_writer_stdout_pollution.sh \
        src/zim_writer.cpp .github/workflows/MainDistributionPipeline.yml
git commit -m "fix(libzim): stop the writer's ungated stdout prints (overlay patch)"
```

---

## Task 9: Documentation

**Files:**
- Modify: `README.md`, `docs/reference.md`, `docs/index.md`, `test/README.md`
- Create: `docs/writing.md`; modify `mkdocs.yml` to list it

**Interfaces:**
- Consumes: the finished v1 surface.
- Produces: no code.

- [ ] **Step 1: Add the reference entry**

In `docs/reference.md`, after the `zim://` filesystem section, add:

````markdown
## Writing archives

### `COPY (query) TO 'out.zim' (FORMAT zim [, options])`

Writes the query's rows into a new ZIM archive.

**Input columns** (resolved by name, case-insensitive):

| Column | Type | Required | Meaning |
|---|---|---|---|
| `path` | VARCHAR | **yes** | entry path; must be unique |
| `content` | VARCHAR \| BLOB | **yes** | entry bytes |
| `title` | VARCHAR | no | defaults to `path` |
| `mimetype` | VARCHAR | no | defaults to `text/plain` |
| `is_redirect` / `redirect_path` | BOOLEAN / VARCHAR | no | write a redirect instead of an item |
| `front_article` / `compress` | BOOLEAN | no | libzim hints |

`size` and `file_path` are accepted and ignored, so `read_zim`'s own output can be
piped straight back in. Any other column is an error.

**Options:** `TITLE`, `DESCRIPTION`, `LANGUAGE`, `CREATOR`, `PUBLISHER`, `NAME`, `DATE`,
`TAGS`, `METADATA` (a `MAP`), `ILLUSTRATION` (48×48 PNG `BLOB`), `MAIN_PATH`, `INDEX`,
`INDEX_LANGUAGE`, `COMPRESSION`, `CLUSTER_SIZE`, `WORKERS`, `ON_CONFLICT`.

> **Writing never overwrites.** If the output path exists the copy fails — unlike
> `parquet`/`csv`, which clobber by default. A ZIM is often the only copy of a corpus that
> took hours to build, and a *failed* write leaves an archive that still opens and still
> passes `zim_check()`. Remove the file yourself if you mean to replace it. On any error
> the partial output is deleted.

```sql
-- a searchable archive from a table
COPY (SELECT path, title, mimetype, content FROM articles)
TO 'out.zim' (FORMAT zim, TITLE 'My Archive', LANGUAGE 'eng', INDEX true);

-- copy an existing archive
COPY (SELECT * FROM read_zim('in.zim', include_content := true))
TO 'copy.zim' (FORMAT zim);
```

**Not yet supported:** `content_path` / `entry_kind` / `target` columns, `PARTITION_BY`,
and aliases (an alias in a source archive is copied as a duplicate item). Each is rejected
with an explanatory error rather than ignored.
````

- [ ] **Step 2: Update the README surface list and status banner**

In `README.md`, change the status banner (line 10) from "the reader is **feature-complete**"
to note that writing has landed, and add a `## Writing` section mirroring the reference
entry above with the two example queries.

- [ ] **Step 3: Update the docs index and test README**

Add `writing.md` to `mkdocs.yml`'s nav, add a one-line pointer in `docs/index.md`, and add
the two new test files to the list in `test/README.md`:

```markdown
- `copy_zim.test`           — `COPY ... TO ... (FORMAT zim)`: write, round trip, metadata, index
- `copy_zim_errors.test`    — write refusals: existing output, duplicates, bad options
```

- [ ] **Step 4: Verify the docs build**

```bash
python3 -m mkdocs build --strict 2>&1 | tail -5
```

Expected: no warnings about missing nav entries or broken links. If mkdocs is not installed,
skip and note it.

- [ ] **Step 5: Final full-suite run**

```bash
make release && ./build/release/test/unittest "test/sql/*" && \
bash test/no_stdout_pollution.sh && bash test/no_writer_stdout_pollution.sh
```

Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add README.md docs/reference.md docs/index.md docs/writing.md mkdocs.yml test/README.md
git commit -m "docs: document COPY ... TO ... (FORMAT zim)"
```

---

## Notes for the executor

**Things that are genuinely uncertain**, flagged so you verify rather than trust this plan:

1. **`zim::Compression` enumerator spelling** (Task 1) — check `zim/zim.h`.
2. **`MapValue::GetChildren` / `StructValue::GetChildren`** (Task 4) — DuckDB has renamed
   these; check `value.hpp` in the pinned submodule.
3. **Whether `PARTITION_BY` reaches `copy_to_bind`** (Task 7) — `bind_copy.cpp` parses it
   into `partition_cols` before the format's bind runs, so it may never appear in
   `info.options`. Task 7 Step 4 covers both outcomes.
4. **`test/oracle/test.zim`'s redirect target** (Task 6) — query it, don't guess.
5. **Whether `__TEST_DIR__` expands inside a `COPY … TO` target** — the sqllogictest runner
   replaces it in query text (`sqllogic_test_runner.cpp`), but no existing test in this repo
   uses it. Verify with Task 1's very first test; if it does not expand, write to a relative
   path under `duckdb_unittest_tempdir/` instead and delete it in the test.

### Deliberate deviations from the spec's test plan

The spec's §10.1 v1 table lists a **libzim private-API property** test (the §8.1
dedup/alias invariant). It is *not* in this plan, deliberately: v1 does not compile with
`ZIM_PRIVATE` and never calls `getClusterIndex()`/`getBlobIndex()`, so the test would guard
a dependency that does not yet exist. It becomes required in whichever version first uses
those accessors for alias preservation. Do not add it here; do not forget it there.

Similarly, `USE_TMP_FILE` (spec §3.2) gets no dedicated rejection — the option loop's final
`else` already rejects every unrecognised option by name, which covers it with a clear
message. If DuckDB handles `USE_TMP_FILE` before our bind sees it, the same caveat as
`PARTITION_BY` applies and it needs the Task 7 Step 4 treatment.

**Do not** weaken a test to make it pass. If a test's expectation turns out wrong, say so
explicitly and change the expectation as its own step with its own reasoning — the golden
rule in `CLAUDE.md` is to change implementation *or* tests, not both at once.

**The unlink-on-error guarantee (Task 2) is the one thing in this plan that must not ship
broken.** Its verification step deliberately removes the cleanup and confirms the tests go
red. Do not skip that step.
