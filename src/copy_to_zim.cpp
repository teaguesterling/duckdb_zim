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
	// v2 columns: named so the deferral reads as a deferral, not as a typo.
	static const char *DEFERRED_COLUMNS[] = {"content_path", "entry_kind", "target", "source_archive", "source_entry"};

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

	// Deliberate deviation from parquet/csv, which clobber by default. A ZIM is
	// often the only copy of a corpus that took hours to build, and a failed write
	// leaves a valid-looking archive (§7.2) -- clobber-by-default plus
	// silent-partial-success destroys data and then reports health. Refusing also
	// subsumes the self-reference hazard: a source archive necessarily exists, so
	// it can never be a valid output path.
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
		throw InvalidInputException("COPY TO (FORMAT zim): output '%s' already exists; refusing to overwrite. "
		                            "A ZIM is written once -- remove the file first if you mean to replace it.",
		                            input.info.file_path);
	}

	// Derive the mimetype default from the content column's SQL type. libzim writes
	// "WARNING: mimetype missing for <path>" to stderr once per row, so defaulting
	// (rather than passing NULL through) is what keeps a large archive's stderr usable.
	bind->default_mimetype = sql_types[static_cast<idx_t>(bind->cols.content)].id() == LogicalTypeId::BLOB
	                             ? "application/octet-stream"
	                             : "text/plain";

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
			// Same VARCHAR guard as the METADATA map branch below: value.ToString() is a
			// formatted cast that \x-escapes bytes >= 0x80 on a BLOB (CastFromBlob). Named
			// metadata options are text by contract, but nothing stops a caller from passing
			// a BLOB literal, so reject that explicitly rather than silently corrupting it.
			if (value.type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("COPY TO (FORMAT zim): %s must be VARCHAR, not %s -- ToString() escapes "
				                      "binary content instead of passing it through",
				                      StringUtil::Upper(option.first), value.type().ToString());
			}
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
			bind->config.main_path = value.ToString();
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
		} else {
			throw BinderException("COPY TO (FORMAT zim): unknown option '%s'", option.first);
		}
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
		if (!GetStringCell(input, bind.cols.title, row, entry.title)) {
			entry.title = entry.path;
		}
		GetStringCell(input, bind.cols.mimetype, row, entry.mimetype);
		GetStringCell(input, bind.cols.content, row, entry.content);

		if (bind.cols.is_redirect >= 0) {
			auto &vec = input.data[static_cast<idx_t>(bind.cols.is_redirect)];
			if (FlatVector::Validity(vec).RowIsValid(row) && FlatVector::GetData<bool>(vec)[row]) {
				entry.is_redirect = true;
				if (!GetStringCell(input, bind.cols.redirect_path, row, entry.redirect_path)) {
					throw InvalidInputException(
					    "COPY TO (FORMAT zim): entry '%s' has is_redirect = true but no redirect_path", entry.path);
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

		// A redirect has no mimetype and no content of its own -- skip the default.
		if (!entry.is_redirect && entry.mimetype.empty()) {
			entry.mimetype = bind.default_mimetype;
		}

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
