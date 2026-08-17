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
	// later against the file_path handed to ZimCopyInitGlobal, even though the
	// task-2 implementation plan says to put it there. Verified empirically that
	// the plan's placement does not work: when the target already exists, DuckDB's
	// planner (bind_copy.cpp) defaults use_tmp_file to true, and physical planning
	// (plan_copy_to_file.cpp) then rewrites the copy operator's file_path to
	// "tmp_<name>" *before* copy_to_initialize_global ever sees it -- so that path
	// (almost) never itself pre-exists, and an existence check there would (almost)
	// never fire. Do not "fix" this back to InitGlobal; see task-2-report.md for
	// the full trace (grep for "Deviation").
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
