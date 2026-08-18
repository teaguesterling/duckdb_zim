//===----------------------------------------------------------------------===//
// copy_to_zim.cpp — COPY ... TO ... (FORMAT zim).
//
// DuckDB binding only: no zim:: types appear here (see zim_writer.hpp).
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_writer.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace duckdb {

using zim_ext::ZimWriteEntry;
using zim_ext::ZimWriter;
using zim_ext::ZimWriterConfig;
using zim_ext::ZimWriterHasFulltextIndexing;

namespace {

// Column indices resolved by name at bind time. -1 means "column absent".
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

// How to handle a duplicate 'path' seen by the sink. 'last' is deliberately not a
// member here -- it would require buffering every row, so it is rejected at bind
// time instead (see ZimCopyBind) rather than represented as a runtime policy.
enum class ZimConflictPolicy { ERROR_ON_DUPLICATE, KEEP_FIRST };

struct ZimCopyBindData : public FunctionData {
	ZimColumns cols;
	ZimWriterConfig config;
	// Default mimetype, derived from the content column's SQL type at bind time.
	string default_mimetype = "text/plain";
	ZimConflictPolicy on_conflict = ZimConflictPolicy::ERROR_ON_DUPLICATE;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ZimCopyBindData>();
		result->cols = cols;
		result->config = config;
		result->default_mimetype = default_mimetype;
		result->on_conflict = on_conflict;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		return this == &other_p;
	}
};

struct ZimCopyGlobalState : public GlobalFunctionData {
	ZimCopyGlobalState(ClientContext &context_p, string path_p) : context(context_p), out_path(std::move(path_p)) {
	}

	~ZimCopyGlobalState() override {
		// Defense in depth, NOT the load-bearing mechanism -- see
		// docs/dev/copy-to-zim-design.md §7.2, which was corrected on this point.
		// libzim writes to "<target>.tmp" and renames to the target name only at the
		// END of finishZimCreation() (creator.cpp:453), and this code never finalizes
		// while unwinding -- so no reachable abort leaves a file at the target name for
		// this unlink to find. THAT is the actual guarantee: a truncated-but-finalized
		// archive would open, checksum and pass zim_check() exactly like a complete one,
		// so it must never be produced in the first place; no post-hoc check can spot
		// it. The unlink below costs nothing and stays correct if a future libzim drops
		// the rename, or if a future code path creates the target early.
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
	// Every redirect entry the sink accepted: its own path -> the non-empty
	// redirect_path it named. Keyed by the REDIRECT's path, not the target's, so
	// the map doubles as "is this path itself a redirect?" -- which is what makes
	// the transitive walk in ZimCopyFinalize possible. Paths are unique (the dedup
	// check runs first), so no entry is ever lost to a collision here. See
	// ZimCopyFinalize and docs/dev/copy-to-zim-design.md §7.4 for why libzim cannot
	// be trusted to catch either dangling or looping redirects itself.
	std::map<string, string> redirects;
	bool finished = false;
};

struct ZimCopyLocalState : public LocalFunctionData {};

// Named metadata options are sugar for METADATA. Each maps to its ZIM key.
const std::map<string, string> METADATA_OPTIONS = {{"title", "Title"},         {"description", "Description"},
                                                   {"language", "Language"},   {"creator", "Creator"},
                                                   {"publisher", "Publisher"}, {"name", "Name"},
                                                   {"date", "Date"},           {"tags", "Tags"}};

void SetMetadata(ZimWriterConfig &config, const string &key, const string &value) {
	if (!config.metadata.insert({key, value}).second) {
		throw BinderException(
		    "COPY TO (FORMAT zim): metadata key '%s' given twice; specify it either as a named option "
		    "or in METADATA, not both",
		    key);
	}
}

// Resolve a column by name, case-insensitively. Returns -1 when absent.
int64_t FindColumn(const vector<string> &names, const string &want) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (StringUtil::CIEquals(names[i], want)) {
			return static_cast<int64_t>(i);
		}
	}
	return -1;
}

unique_ptr<FunctionData> ZimCopyBind(ClientContext &context, CopyFunctionBindInput &input, const vector<string> &names,
                                     const vector<LogicalType> &sql_types) {
	auto bind = make_uniq<ZimCopyBindData>();

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
	// v2 columns: named so the deferral reads as a deferral, not as a typo. Only
	// columns design §10 actually plans belong here. `source_archive`/`source_entry`
	// were the four-column alternative §4.2 explicitly REJECTED in favour of the
	// single `zim://` locator, so naming them here would promise a v2 that will not
	// exist -- they are unknown columns, and fail as such.
	static const char *DEFERRED_COLUMNS[] = {"content_path", "entry_kind", "target"};

	for (idx_t i = 0; i < names.size(); i++) {
		auto &n = names[i];
		bool known = false;
		for (auto *k :
		     {"path", "content", "title", "mimetype", "is_redirect", "redirect_path", "front_article", "compress"}) {
			known |= StringUtil::CIEquals(n, k);
		}
		for (auto *k : IGNORED_COLUMNS) {
			known |= StringUtil::CIEquals(n, k);
		}
		for (auto *k : DEFERRED_COLUMNS) {
			if (StringUtil::CIEquals(n, k)) {
				throw BinderException("COPY TO (FORMAT zim): column '%s' is not supported yet (planned for the next "
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

	// Validate the SQL type of every column the sink actually reads, here, where
	// sql_types is in hand. Without this the mismatch is caught by
	// FlatVector::GetData<string_t>/<bool> in the sink, which throws an
	// InternalException -- mid-write, after the archive has been started, and with
	// no SQL-level explanation, because DuckDB invalidates the database on
	// ExceptionType::INTERNAL (client_context.cpp). Design §4.1 tabulates the
	// intended type for every column; this enforces that table.
	auto require_type = [&](int64_t col, const char *col_name, bool allow_blob, bool want_boolean) {
		if (col < 0) {
			return;
		}
		auto &type = sql_types[static_cast<idx_t>(col)];
		bool ok = want_boolean
		              ? type.id() == LogicalTypeId::BOOLEAN
		              : (type.id() == LogicalTypeId::VARCHAR || (allow_blob && type.id() == LogicalTypeId::BLOB));
		if (!ok) {
			throw BinderException("COPY TO (FORMAT zim): column '%s' must be %s, not %s. Cast it in the query.",
			                      col_name, want_boolean ? "BOOLEAN" : (allow_blob ? "VARCHAR or BLOB" : "VARCHAR"),
			                      type.ToString());
		}
	};
	require_type(bind->cols.path, "path", false, false);
	require_type(bind->cols.content, "content", true, false);
	require_type(bind->cols.title, "title", false, false);
	require_type(bind->cols.mimetype, "mimetype", false, false);
	require_type(bind->cols.redirect_path, "redirect_path", false, false);
	require_type(bind->cols.is_redirect, "is_redirect", false, true);
	require_type(bind->cols.front_article, "front_article", false, true);
	require_type(bind->cols.compress, "compress", false, true);

	// Deliberate deviation from parquet/csv, which clobber by default. A ZIM is
	// often the only copy of a corpus that took hours to build, and the format
	// records no "this is incomplete" marker -- a truncated-but-finalized archive
	// opens, checksums and passes zim_check() exactly like a complete one (§7.2).
	// Clobbering would therefore destroy the known-good copy before anything can
	// establish that the replacement is whole. Refusing also subsumes the
	// self-reference hazard: a source archive necessarily exists, so it can never be
	// a valid output path.
	//
	// This check must run here, at bind time, against input.info.file_path -- NOT
	// later against the file_path handed to ZimCopyInitGlobal, even though an
	// earlier draft of the implementation plan said to put it there. Verified
	// empirically that placement does not work: when the target already exists,
	// DuckDB's planner (bind_copy.cpp) defaults use_tmp_file to true, and physical
	// planning (plan_copy_to_file.cpp) then rewrites the copy operator's file_path
	// to "tmp_<name>" *before* copy_to_initialize_global ever sees it -- so that
	// path (almost) never itself pre-exists, and an existence check there would
	// (almost) never fire. Do not "fix" this back to InitGlobal.
	auto &fs = FileSystem::GetFileSystem(context);
	if (fs.FileExists(input.info.file_path)) {
		// The message names OVERWRITE and friends explicitly because DuckDB's binder
		// accepts them for every format and consumes them before copy_to_bind runs, so
		// this refusal fires even when the caller asked for exactly this -- and "remove
		// the file first" then reads as an instruction to do by hand what they just
		// requested. They are inert here, and the message has to say so (§3.4 keeps
		// refusal as the behaviour; only the wording changes).
		throw InvalidInputException(
		    "COPY TO (FORMAT zim): output '%s' already exists; refusing to overwrite. A ZIM is written "
		    "once -- remove the file first if you mean to replace it. OVERWRITE, OVERWRITE_OR_IGNORE and "
		    "APPEND are not supported for this format: DuckDB accepts them at the COPY level, but they "
		    "have no effect on a zim target and do not lift this refusal.",
		    input.info.file_path);
	}

	// The target is not the only file this write claims. libzim writes the whole
	// archive to "<target>.tmp" and renames it into place at the end, and it opens
	// that file with O_CREAT|O_TRUNC and NO O_EXCL (creator.cpp:602) -- so a second
	// writer aimed at the same target silently truncates and interleaves into the
	// first one's buffer, and whichever finishes last renames the result over the
	// target. The product is a corrupt archive that (§7.2) still opens, checksums and
	// passes zim_check(). Refusing when the ".tmp" is already there turns the common
	// case of that -- a concurrent write, or the leftovers of a crashed one -- into an
	// error a user can act on.
	//
	// KNOWN LIMITATION, deliberately not claimed as a fix: this is TOCTOU. The whole
	// planning phase separates it from startZimCreation(), so two COPYs can both pass
	// it and then both create the ".tmp". Closing the race for real needs O_EXCL
	// inside libzim, which is out of scope here. This narrows the window and reports
	// the leftovers; it does not make concurrent COPY to one target safe. See
	// docs/dev/copy-to-zim-design.md §3.4.
	auto tmp_path = input.info.file_path + ".tmp";
	if (fs.FileExists(tmp_path)) {
		throw InvalidInputException(
		    "COPY TO (FORMAT zim): '%s' already exists, so a write to '%s' is either in progress in another "
		    "connection or was interrupted partway through. libzim builds the archive there and renames it "
		    "into place at the end, and it does not lock the file -- continuing would interleave two writes "
		    "into one buffer and produce a corrupt archive. Wait for the other write, or remove '%s' if "
		    "nothing is writing it.",
		    tmp_path, input.info.file_path, tmp_path);
	}

	// Derive the mimetype default from the content column's SQL type. libzim writes
	// "WARNING: mimetype missing for <path>" to stderr once per row, so defaulting
	// (rather than passing NULL through) is what keeps a large archive's stderr usable.
	bind->default_mimetype = sql_types[static_cast<idx_t>(bind->cols.content)].id() == LogicalTypeId::BLOB
	                             ? "application/octet-stream"
	                             : "text/plain";

	// Value::ToString() is a FORMATTED cast, not a raw extraction: on a BLOB it goes
	// through CastFromBlob and \x-escapes every byte >= 0x80. So every option whose
	// value is read with ToString() has to establish that the value really is VARCHAR
	// first, or a BLOB argument is silently mangled rather than rejected -- e.g.
	// MAIN_PATH encode('A/café') became the 12-character 'A/caf\xC3\xA9' and then
	// failed ZimCopyFinalize's check with a message naming a path the caller never
	// typed. Shared by every such branch so a new option cannot forget it.
	auto require_varchar = [](const Value &value, const string &option_name) {
		if (value.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("COPY TO (FORMAT zim): %s must be VARCHAR, not %s -- ToString() escapes "
			                      "binary content instead of passing it through",
			                      StringUtil::Upper(option_name), value.type().ToString());
		}
	};

	// DuckDB's binder consumes every DuckDB-level COPY option (partition_by,
	// overwrite, etc.) before we get here, and strips FORMAT too -- so
	// input.info.options holds only options unrecognised at that level. This loop
	// is the single entry point later tasks extend with more `else if` branches.
	for (auto &option : input.info.options) {
		auto key = StringUtil::Lower(option.first);
		if (option.second.size() != 1) {
			throw BinderException("COPY TO (FORMAT zim): option '%s' takes exactly one value", option.first);
		}
		auto &value = option.second[0];
		// A NULL Value must never reach the branches below. StringValue::Get and
		// GetValue<uint64_t>() throw InternalException on one (value.cpp), which
		// invalidates the database rather than reporting a SQL error; BooleanValue::Get
		// reaches GetValueUnsafe<bool>, which reads value_.boolean behind only a
		// D_ASSERT -- an uninitialized read in a release build, so `INDEX <null>` would
		// resolve to garbage; and ToString() returns the literal string "NULL", so
		// `MAIN_PATH <null>` would ask for an entry named "NULL". Same bug class as
		// issue #28 on the read side (see NonNegativeParam/BoolParam in zim_search.cpp).
		//
		// DuckDB's own binder rejects a directly-spelled NULL first
		// (bind_copy.cpp BindCopyOption), but it does that check BEFORE unpacking an
		// unnamed struct into several option values -- so `WORKERS row(NULL)` reaches
		// here with a NULL Value today. This guard is the backstop that does not depend
		// on which spellings DuckDB happens to filter.
		if (value.IsNull()) {
			throw BinderException("COPY TO (FORMAT zim): %s must not be NULL", StringUtil::Upper(option.first));
		}
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
		} else if (METADATA_OPTIONS.count(key)) {
			// Named metadata options are text by contract, but nothing stops a caller from
			// passing a BLOB literal, so reject that explicitly rather than corrupting it.
			require_varchar(value, option.first);
			SetMetadata(bind->config, METADATA_OPTIONS.at(key), value.ToString());
		} else if (key == "metadata") {
			if (value.type().id() != LogicalTypeId::MAP) {
				throw BinderException("COPY TO (FORMAT zim): METADATA must be a MAP(VARCHAR, VARCHAR)");
			}
			// Value's own values.ToString() is a formatted cast, not a raw extraction: on
			// a BLOB it goes through CastFromBlob and \x-escapes every byte >= 0x80. That
			// is silently wrong for binary metadata, so the map's value type must be
			// VARCHAR -- checked once here, since a MAP's value type is homogeneous across
			// every entry.
			if (MapType::ValueType(value.type()).id() != LogicalTypeId::VARCHAR) {
				throw BinderException(
				    "COPY TO (FORMAT zim): METADATA values must be VARCHAR, not %s -- ToString() escapes "
				    "binary content instead of passing it through",
				    MapType::ValueType(value.type()).ToString());
			}
			auto &entries = MapValue::GetChildren(value);
			for (auto &entry : entries) {
				auto &kv = StructValue::GetChildren(entry);
				// The option-level NULL guard above cannot see inside the map: the map
				// Value itself is non-NULL while an individual value is NULL, and
				// ToString() then writes the literal string "NULL" as that key's
				// metadata. (Map KEYS cannot be NULL -- DuckDB rejects that itself.)
				if (kv[1].IsNull()) {
					throw BinderException("COPY TO (FORMAT zim): METADATA value for key '%s' must not be NULL",
					                      kv[0].ToString());
				}
				SetMetadata(bind->config, kv[0].ToString(), kv[1].ToString());
			}
		} else if (key == "illustration") {
			// Raw bytes, not value.ToString(): ToString() on a BLOB goes through
			// CastFromBlob and \x-escapes every byte >= 0x80, which corrupts a real PNG
			// (binary, non-ASCII-heavy). StringValue::Get returns the underlying bytes
			// with no cast -- the same thing GetStringCell does for row data below, just
			// for a bound Value instead of a DataChunk cell. It requires PhysicalType::
			// VARCHAR (true for both VARCHAR and BLOB; see LogicalType::GetInternalType),
			// so reject anything else explicitly rather than relying on its debug-only
			// assertion.
			if (value.type().id() != LogicalTypeId::VARCHAR && value.type().id() != LogicalTypeId::BLOB) {
				throw BinderException("COPY TO (FORMAT zim): ILLUSTRATION must be VARCHAR or BLOB, not %s",
				                      value.type().ToString());
			}
			bind->config.illustration = StringValue::Get(value);
		} else if (key == "main_path") {
			// An entry path is text; a BLOB here would be \x-escaped into a path that
			// matches nothing, and the failure would surface from ZimCopyFinalize as
			// "MAIN_PATH '...' does not match any entry" naming a mangled string.
			require_varchar(value, option.first);
			bind->config.main_path = value.ToString();
		} else if (key == "index") {
			bind->config.index = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (key == "index_language") {
			// A mangled stemmer language does not fail at all -- libzim takes whatever
			// string it is handed -- so an escaped BLOB would just silently configure the
			// wrong stemmer.
			require_varchar(value, option.first);
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
			// 0 is the sentinel ZimWriterConfig uses internally for "leave libzim's
			// default alone", so accepting a literal CLUSTER_SIZE 0 would silently
			// ignore the option instead of honouring it. There is no meaningful
			// zero-byte cluster target either way; reject it and say what to do instead.
			auto n = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
			if (n == 0) {
				throw BinderException("COPY TO (FORMAT zim): CLUSTER_SIZE must be at least 1 byte. Omit the "
				                      "option entirely to use libzim's own default.");
			}
			bind->config.cluster_size = n;
		} else if (key == "workers") {
			// The upper bound is a sanity check, not a libzim limit: each worker is a
			// thread, so a fat-fingered WORKERS 1000000 would try to spawn a million of
			// them inside libzim rather than fail with a SQL error.
			static constexpr uint64_t MAX_WORKERS = 256;
			auto n = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
			if (n == 0 || n > MAX_WORKERS) {
				throw BinderException("COPY TO (FORMAT zim): WORKERS must be between 1 and %llu",
				                      static_cast<unsigned long long>(MAX_WORKERS));
			}
			bind->config.workers = static_cast<uint32_t>(n);
		} else {
			throw BinderException("COPY TO (FORMAT zim): unknown option '%s'", option.first);
		}
	}

	// The mirror of the INDEX-without-a-language error below. ZimWriter only calls
	// configIndexing() when config.index is set, so INDEX_LANGUAGE on its own writes
	// an archive with no fulltext index at all -- measured: zim_search() returns zero
	// rows against it, with no error anywhere. Someone who named a stemming language
	// asked for a searchable archive, and got the same unsearchable one they would
	// have got by saying nothing.
	//
	// Erroring is the fix, NOT implying INDEX true. Turning indexing on because a
	// language was mentioned is its own surprise (indexing costs time and bytes), and
	// this design does not silently do more than it was asked to any more than it
	// silently does less. This check must run BEFORE the LANGUAGE-metadata defaulting
	// below, which would otherwise make its condition unreadable.
	if (!bind->config.index_language.empty() && !bind->config.index) {
		throw BinderException("COPY TO (FORMAT zim): INDEX_LANGUAGE has no effect without INDEX true -- it only "
		                      "chooses the stemming language for a fulltext index that is not being built. Pass "
		                      "INDEX true as well to write one, or drop INDEX_LANGUAGE.");
	}

	// Indexing is off unless requested (design §3.1). A requested index with no
	// language is an error, not a silent no-op -- an unsearchable archive that was
	// asked to be searchable is the silent-failure shape this design keeps
	// guarding against.
	if (bind->config.index && bind->config.index_language.empty()) {
		// Default the index language from LANGUAGE metadata when it was given.
		auto lang = bind->config.metadata.find("Language");
		if (lang != bind->config.metadata.end()) {
			bind->config.index_language = lang->second;
		}
	}
	if (bind->config.index && bind->config.index_language.empty()) {
		throw BinderException("COPY TO (FORMAT zim): INDEX requires a language for stemming. Pass LANGUAGE 'eng' "
		                      "(which also sets the Language metadata) or INDEX_LANGUAGE 'eng'.");
	}
	if (bind->config.index && !ZimWriterHasFulltextIndexing()) {
		throw BinderException("COPY TO (FORMAT zim): INDEX true was requested, but this build has no Xapian support "
		                      "(WebAssembly builds are search-less), so no fulltext index can be written. Omit INDEX "
		                      "to write an archive without one.");
	}

	return std::move(bind);
}

unique_ptr<LocalFunctionData> ZimCopyInitLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq<ZimCopyLocalState>();
}

unique_ptr<GlobalFunctionData> ZimCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                 const string &file_path) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();

	// The "already exists" refusal lives in ZimCopyBind, not here -- see the
	// comment there for why this file_path is the wrong value to check against.
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

		// Duplicate detection runs FIRST -- before any other per-row validation, and
		// before anything about this row is recorded in the global state. A row that
		// ON_CONFLICT 'first' skips is never written, so it must not register a
		// redirect target either: doing that made ZimCopyFinalize reject the whole
		// COPY over an entry that was deliberately dropped.
		if (!gstate.seen_paths.insert(entry.path).second) {
			if (bind.on_conflict == ZimConflictPolicy::KEEP_FIRST) {
				continue;
			}
			throw InvalidInputException(
			    "COPY TO (FORMAT zim): duplicate path '%s'. Entry paths must be unique within an "
			    "archive; deduplicate in SQL, or pass ON_CONFLICT 'first'.",
			    entry.path);
		}

		if (!GetStringCell(input, bind.cols.title, row, entry.title)) {
			entry.title = entry.path;
		}
		GetStringCell(input, bind.cols.mimetype, row, entry.mimetype);

		if (bind.cols.is_redirect >= 0) {
			auto &vec = input.data[static_cast<idx_t>(bind.cols.is_redirect)];
			if (FlatVector::Validity(vec).RowIsValid(row) && FlatVector::GetData<bool>(vec)[row]) {
				entry.is_redirect = true;
				if (!GetStringCell(input, bind.cols.redirect_path, row, entry.redirect_path)) {
					throw InvalidInputException(
					    "COPY TO (FORMAT zim): entry '%s' has is_redirect = true but no redirect_path", entry.path);
				}
				// Record this redirect for the dangling/cycle checks in ZimCopyFinalize.
				// Keyed by this entry's own path, which the dedup check above has already
				// established is unique -- so this records every redirect, not just the
				// first to name a given target. This is below the dedup check on purpose
				// (see the comment there).
				gstate.redirects.emplace(entry.path, entry.redirect_path);
			}
		}

		// Content is read after is_redirect is known, because whether NULL is legal
		// depends on it. A redirect has no content of its own; anything else with NULL
		// content would be written as an EMPTY entry -- silently, with no error and no
		// size difference a content-comparison test could catch. That is the exact
		// failure docs/writing.md calls "the single easiest way to lose data with this
		// feature" (it is what feeding a COPY from content_as_varchar := true does to
		// every non-UTF-8 entry), so it is an error naming the row, per design §4.2.
		if (!GetStringCell(input, bind.cols.content, row, entry.content) && !entry.is_redirect) {
			throw InvalidInputException(
			    "COPY TO (FORMAT zim): entry '%s' has NULL content. Only a redirect row (is_redirect = true) "
			    "may have NULL content -- writing it as an empty entry would lose the data silently. If the "
			    "source is read_zim(), drop content_as_varchar := true and read content as BLOB: it returns "
			    "NULL for every entry whose bytes are not valid UTF-8.",
			    entry.path);
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

		// A redirect has no mimetype and no content of its own -- skip the default.
		if (!entry.is_redirect && entry.mimetype.empty()) {
			entry.mimetype = bind.default_mimetype;
		}

		gstate.writer->AddItem(entry);
	}
}

void ZimCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                    LocalFunctionData &lstate) {
}

void ZimCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) {
	auto &bind = bind_data.Cast<ZimCopyBindData>();
	auto &gstate = gstate_p.Cast<ZimCopyGlobalState>();

	// libzim accepts a main path that was never added and silently produces an
	// archive with has_main_entry = false (§7.3). Validate against what we wrote.
	if (!bind.config.main_path.empty() && !gstate.seen_paths.count(bind.config.main_path)) {
		throw InvalidInputException("COPY TO (FORMAT zim): MAIN_PATH '%s' does not match any entry in the input",
		                            bind.config.main_path);
	}

	// libzim accepts addRedirection() to a target that was never added and
	// silently REMOVES the dangling redirect at finishZimCreation() -- no throw,
	// no warning (design §7.4). The only signal was two stdout prints that this
	// task's overlay patch (vcpkg_ports/libzim/no-writer-stdout.patch) deletes
	// along with the four that fire on every write, which would make the drop
	// completely silent. Validate every redirect target against what was
	// actually written, same as MAIN_PATH above, so this is a clear SQL error
	// instead of a vanished entry. Order does not matter: this runs after every
	// row has been seen, so a redirect declared before its target is fine.
	for (auto &kv : gstate.redirects) {
		auto &source_path = kv.first;
		auto &target = kv.second;
		if (!gstate.seen_paths.count(target)) {
			throw InvalidInputException(
			    "COPY TO (FORMAT zim): redirect '%s' -> '%s' does not match any entry in the input", source_path,
			    target);
		}
	}

	// Pointing at a path that exists is NOT enough: libzim runs
	// removeLoopsAndBlindChainsOfRedirects() (creator.cpp:851) right after
	// detectDanglingRedirects(), and it markRemoved()s every redirect in a chain
	// that does not terminate at a real item -- a cycle, or a chain of redirects
	// all of which are themselves removed. That drop is as silent as the dangling
	// one, and for the same reason: its only announcement was an INFO() print the
	// stdout patch deletes. Measured before this check: writing an item plus
	// 'A/X' -> 'A/Y' and 'A/Y' -> 'A/X' produced an archive containing only the
	// item, with no error and no warning -- two input rows gone.
	//
	// Every redirect must therefore resolve TRANSITIVELY to a path that is not
	// itself a redirect. The loop below is not unbounded: `on_chain` makes each walk
	// visit any given redirect at most once (so a walk is at most redirects.size()
	// steps before it either terminates or is reported as a cycle), and `terminates`
	// memoizes proven-good redirects so the total work stays linear in the number of
	// redirects however deeply they are chained. A legitimately long chain is
	// accepted, not rejected on an arbitrary hop limit.
	std::set<string> terminates; // redirects already proven to end at a real item
	for (auto &start : gstate.redirects) {
		vector<string> chain; // redirects walked on this pass, in order
		std::set<string> on_chain;
		string current = start.first;
		while (true) {
			if (terminates.count(current)) {
				break; // this tail was already proven good
			}
			auto next = gstate.redirects.find(current);
			if (next == gstate.redirects.end()) {
				break; // a real item: the chain terminates here
			}
			if (!on_chain.insert(current).second) {
				// Back on a path this same walk already visited -- report the cycle
				// itself, not the arbitrary entry the outer loop happened to start from.
				string cycle;
				for (auto it = std::find(chain.begin(), chain.end(), current); it != chain.end(); ++it) {
					cycle += "'" + *it + "' -> ";
				}
				cycle += "'" + current + "'";
				throw InvalidInputException(
				    "COPY TO (FORMAT zim): redirect '%s' is part of a redirect cycle (%s). A redirect chain "
				    "must end at an entry that is not itself a redirect; libzim removes every redirect in a "
				    "cycle, dropping those rows from the archive with no error.",
				    current, cycle);
			}
			chain.push_back(current);
			current = next->second;
		}
		// Everything walked to get here reaches a real item, so record the whole tail.
		for (auto &path : chain) {
			terminates.insert(path);
		}
	}

	gstate.writer->Finish();

	// Finish() returning is NOT proof that an archive was written. finishZimCreation()
	// ends with DEFAULTFS::rename("<target>.tmp", "<target>"), and libzim's FS::rename
	// (src/fs_unix.cpp:103) calls ::rename() and DISCARDS its return value -- no throw,
	// no status, nothing. So a failed rename is indistinguishable from a successful one
	// from inside libzim, and COPY would report success over an output that does not
	// exist. That is reachable from this extension's own error paths: PhysicalCopyToFile
	// ::GetGlobalSinkState CreateDirectory()s the target before initialize_operator can
	// refuse PER_THREAD_OUTPUT, so the obvious retry without that option targets a path
	// that is now a DIRECTORY -- ::rename() fails with EISDIR/ENOTDIR and the copy
	// "succeeds" having written nothing.
	//
	// Check the file is really there, and throw BEFORE setting `finished`, so the
	// destructor's cleanup still runs on this path.
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(gstate.out_path)) {
		// libzim clears tmpFileName immediately after the rename call (creator.cpp:457),
		// so ~CreatorData's own "remove the .tmp" (creator.cpp:644) no longer fires and
		// the half-written sibling would be left behind. Drop libzim's handle first, then
		// remove it ourselves. A failure to clean up must not mask the real error below.
		gstate.writer.reset();
		try {
			auto tmp_path = gstate.out_path + ".tmp";
			if (fs.FileExists(tmp_path)) {
				fs.RemoveFile(tmp_path);
			}
		} catch (const std::exception &) { // NOLINT: the throw below is the real error
		}
		throw IOException("COPY TO (FORMAT zim): no archive was written to '%s'. libzim finished the archive "
		                  "but its final rename from '%s.tmp' did not produce that file, and libzim discards "
		                  "the return value of ::rename() so it cannot report why. The usual cause is that "
		                  "the output path is a directory, or is not writable. Nothing was left behind.",
		                  gstate.out_path, gstate.out_path);
	}

	// Set `finished` the instant the output has been confirmed, BEFORE reset(). Once
	// finishZimCreation() has returned and the file is there, the archive on disk is
	// complete, so nothing after this point may lead to it being unlinked. ~Creator is
	// effectively noexcept, so a throw from reset() is not reachable -- but ordering it
	// this way costs nothing and removes the question.
	gstate.finished = true; // suppresses the destructor's unlink
	gstate.writer.reset();
}

// A single Creator is not documented as safe for concurrent addItem(), and entry
// order carries no contract we need. Parallelism comes from libzim's own workers
// (configNbWorkers), not from DuckDB.
CopyFunctionExecutionMode ZimCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

// PARTITION_BY is consumed by DuckDB's binder (bind_copy.cpp clears
// stmt.info->options after extracting it) and never reaches copy_to_bind, so the
// rejection has to happen where the physical operator is visible. This is the
// PhysicalCopyToFile mechanism (not the hive-segment-in-path fallback the task
// plan allowed for): PhysicalCopyToFile and its partition_columns field are
// includable from an extension build against this vendored duckdb, so the more
// precise check was used. Writing one archive per key is planned, but a
// partition can fragment into several archives past
// partitioned_write_max_open_files, and MAIN_PATH cannot survive partitioning --
// neither is handled in this version, so refuse rather than produce a
// surprising result.
void ZimCopyInitializeOperator(GlobalFunctionData &gstate, const PhysicalOperator &op) {
	auto &copy_op = op.Cast<PhysicalCopyToFile>();
	if (!copy_op.partition_columns.empty()) {
		throw NotImplementedException(
		    "COPY TO (FORMAT zim): PARTITION_BY is not supported yet. Writing one archive per key is "
		    "planned, but a partition can fragment into several archives and MAIN_PATH cannot survive "
		    "partitioning, so it needs handling this version does not have.");
	}
	// PER_THREAD_OUTPUT is consumed by the same binder pass and reaches us the same
	// way. It must be refused rather than ignored: PhysicalCopyToFile creates one
	// GlobalFunctionData -- one ZimWriter -- PER THREAD and finalizes each
	// separately, so it would silently produce data_0.zim, data_1.zim, ... each with
	// its own seen_paths (duplicate detection defeated ACROSS files), its own
	// MAIN_PATH validation, and a duplicate copy of the metadata. That is precisely
	// the fragmentation §3.3 refuses PARTITION_BY for, with no key to explain it.
	if (copy_op.per_thread_output) {
		throw NotImplementedException(
		    "COPY TO (FORMAT zim): PER_THREAD_OUTPUT is not supported. It would write one archive per "
		    "thread, each with its own duplicate-path detection, its own MAIN_PATH validation and its own "
		    "copy of the metadata -- an arbitrary split of one corpus across files, which this format has "
		    "no way to express. Write a single archive instead.");
	}
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
	function.initialize_operator = ZimCopyInitializeOperator;
	// function.rotate_files is deliberately left unset (nullptr). DuckDB's own
	// bind_copy.cpp rejects FILE_SIZE_BYTES with a clear, format-named
	// NotImplementedException whenever rotate_files is unset -- assigning a stub
	// here (even one that always returns false) would make that check pass and
	// let execution fall through into rotation logic that then fails differently
	// and worse. Do not add a ZimCopyRotateFiles function.
	function.extension = "zim";
	loader.RegisterFunction(function);
}

} // namespace duckdb
