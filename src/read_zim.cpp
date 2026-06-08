//===----------------------------------------------------------------------===//
// read_zim.cpp — the read_zim table function.
//
// One row per content (C-namespace) entry. Metadata lives in read_zim_metadata.
// Conventions mirror the markdown/yaml/webbed family: positional file arg,
// named `:=` params, projection pushdown, lazy content.
//
// Verified-semantics dependencies (docs/libzim-semantics.md):
//   * scan universe = content entries (iterByPath/iterByTitle)
//   * no `namespace` column (A//I/ are path text, not namespaces)
//   * content is BLOB, loaded only when include_content AND the column is projected
//
// NOTE: this translation unit is the zim<->DuckDB binding. Exact DuckDB API
// spellings (TableFunctionInitInput fields, Value getters) are stable across
// recent versions but should be compiled against the pinned DuckDB. The libzim
// access is entirely via zim_ext::ZimArchive, so nothing here needs <zim/*>.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

#include <memory>
#include <vector>

namespace duckdb {

using zim_ext::ArchivePool;
using zim_ext::ScanOrder;
using zim_ext::ScanSpec;
using zim_ext::ZimArchive;
using zim_ext::ZimEntry;
using zim_ext::ZimScanCursor;

namespace {

// A VARCHAR cell must hold valid UTF-8; binary bytes would corrupt the column and
// trip DuckDB's UTF-8 verification. Callers emit NULL instead of mangling — the same
// "never mangle" policy zim_get_text uses.
static bool IsValidUtf8(const std::string &s) {
	return Utf8Proc::IsValid(s.data(), s.size());
}

// Column layout. file_path is appended only when requested; keep its index last.
enum ZimColumn : idx_t {
	COL_PATH = 0,
	COL_TITLE,
	COL_MIMETYPE,
	COL_IS_REDIRECT,
	COL_REDIRECT_PATH,
	COL_SIZE,
	COL_CONTENT,
	COL_FILE_PATH, // present only if include_filepath
	COL_COUNT
};

struct ReadZimBindData : public TableFunctionData {
	std::vector<std::string> file_paths; // one or more archives (glob/list expanded)
	// resolved options
	bool include_content = false;
	bool content_as_varchar = false;
	bool include_filepath = false;
	ScanSpec spec;
	// exact-lookup mode (path:= or title:=) emits 0/1 row instead of a scan
	bool single_lookup = false;
	bool lookup_by_title = false;
	std::string lookup_key;
	idx_t column_count = COL_FILE_PATH; // becomes COL_COUNT if include_filepath
};

struct ReadZimGlobalState : public GlobalTableFunctionState {
	std::vector<std::string> files;        // archives to scan, in order
	idx_t file_idx = 0;                    // index of the archive currently being read
	std::shared_ptr<ZimArchive> archive;   // current archive (reassigned per file)
	std::unique_ptr<ZimScanCursor> cursor; // current cursor (scan mode); null otherwise
	bool lookup_emitted = false;           // single_lookup: emitted for the current file?
	std::vector<column_t> column_ids;      // projection
	bool want_content = false;             // content projected AND include_content
	// MaxThreads()==1: the file_idx/cursor state machine is sequential and not
	// thread-safe. Parallel scan (phase: partition index ranges) is deferred.
	idx_t MaxThreads() const override {
		return 1;
	}
};

LogicalType ContentType(const ReadZimBindData &bind) {
	return bind.content_as_varchar ? LogicalType::VARCHAR : LogicalType::BLOB;
}

unique_ptr<FunctionData> ReadZimBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadZimBindData>();

	// Input is a single VARCHAR pattern or a LIST(VARCHAR) of patterns. Glob patterns
	// expand via the FileSystem; literal paths pass through untouched so a missing file
	// still surfaces the clean "failed to open ZIM" error at scan time.
	vector<string> patterns;
	auto &arg = input.inputs[0];
	if (arg.type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(arg)) {
			patterns.push_back(child.GetValue<string>());
		}
	} else {
		patterns.push_back(arg.GetValue<string>());
	}
	auto &fs = FileSystem::GetFileSystem(context);
	for (auto &pat : patterns) {
		if (FileSystem::HasGlob(pat)) {
			for (auto &info : fs.Glob(pat)) {
				bind->file_paths.push_back(info.path);
			}
		} else {
			bind->file_paths.push_back(pat);
		}
	}
	if (bind->file_paths.empty()) {
		throw BinderException("read_zim: no files matched the given pattern(s)");
	}

	// Track which mode-selecting params were given so conflicting combinations can be
	// rejected instead of silently resolving last-wins.
	bool has_path = false;
	bool has_title = false;
	bool has_listing = false;

	for (auto &kv : input.named_parameters) {
		auto &key = kv.first;
		auto &val = kv.second;
		if (key == "include_content") {
			bind->include_content = BooleanValue::Get(val);
		} else if (key == "content_as_varchar") {
			bind->content_as_varchar = BooleanValue::Get(val);
		} else if (key == "include_filepath" || key == "filename") {
			bind->include_filepath = BooleanValue::Get(val);
		} else if (key == "mimetype") {
			bind->spec.mimetype = val.GetValue<string>();
		} else if (key == "path_prefix") {
			bind->spec.path_prefix = val.GetValue<string>();
		} else if (key == "title_prefix") {
			bind->spec.title_prefix = val.GetValue<string>();
		} else if (key == "path") {
			has_path = true;
			bind->single_lookup = true;
			bind->lookup_by_title = false;
			bind->lookup_key = val.GetValue<string>();
		} else if (key == "title") {
			has_title = true;
			bind->single_lookup = true;
			bind->lookup_by_title = true;
			bind->lookup_key = val.GetValue<string>();
		} else if (key == "listing") {
			// Picks which libzim listing to traverse, not merely an ordering:
			// 'path' = all user entries (path order); 'title' = the front-article
			// title listing (iterByTitle yields only FRONT_ARTICLE entries).
			has_listing = true;
			auto o = StringUtil::Lower(val.GetValue<string>());
			if (o == "title") {
				bind->spec.order = ScanOrder::ByTitle;
			} else if (o == "path") {
				bind->spec.order = ScanOrder::ByPath;
			} else {
				throw BinderException("read_zim: listing must be 'path' or 'title'");
			}
		} else {
			throw BinderException("read_zim: unknown parameter '%s'", key);
		}
	}

	// Mode validation. Exact lookup (path/title) and listing/prefix scanning are
	// mutually exclusive, and the two exact keys / two prefixes can't be mixed. These
	// combinations used to silently resolve last-wins; reject them instead.
	const bool has_path_prefix = bind->spec.path_prefix.has_value();
	const bool has_title_prefix = bind->spec.title_prefix.has_value();
	if (has_path && has_title) {
		throw BinderException("read_zim: specify only one of path or title");
	}
	if (has_path_prefix && has_title_prefix) {
		throw BinderException("read_zim: specify only one of path_prefix or title_prefix");
	}
	if ((has_path || has_title) && (has_path_prefix || has_title_prefix || has_listing)) {
		throw BinderException("read_zim: exact lookup (path/title) cannot be combined with "
		                      "path_prefix/title_prefix/listing");
	}
	// At this point spec.order still holds the explicit listing (if any); a prefix that
	// contradicts it is an error rather than a silent override.
	if (has_listing && has_path_prefix && bind->spec.order == ScanOrder::ByTitle) {
		throw BinderException("read_zim: listing := 'title' conflicts with path_prefix");
	}
	if (has_listing && has_title_prefix && bind->spec.order == ScanOrder::ByPath) {
		throw BinderException("read_zim: listing := 'path' conflicts with title_prefix");
	}

	// A title_prefix implies title order; a path_prefix implies path order (validated
	// above to agree with any explicit listing).
	if (has_title_prefix) {
		bind->spec.order = ScanOrder::ByTitle;
	} else if (has_path_prefix) {
		bind->spec.order = ScanOrder::ByPath;
	}

	names = {"path", "title", "mimetype", "is_redirect", "redirect_path", "size", "content"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                LogicalType::VARCHAR, LogicalType::UBIGINT, ContentType(*bind)};
	if (bind->include_filepath) {
		names.emplace_back("file_path");
		return_types.emplace_back(LogicalType::VARCHAR);
		bind->column_count = COL_COUNT;
	} else {
		bind->column_count = COL_FILE_PATH;
	}
	return std::move(bind);
}

// Opens files[file_idx] as the current archive; in scan mode also builds the cursor.
// May throw (missing/corrupt archive) — surfaced as the query's error.
static void OpenCurrentFile(ReadZimGlobalState &g, const ReadZimBindData &bind) {
	g.archive = ArchivePool::Instance().Get(g.files[g.file_idx]); // may throw
	g.lookup_emitted = false;
	if (!bind.single_lookup) {
		ScanSpec spec = bind.spec;
		spec.want_content = g.want_content;
		g.cursor = make_uniq<ZimScanCursor>(g.archive->Scan(spec));
	}
}

// Advance to the next archive (opening it), or release state once exhausted.
static void AdvanceFile(ReadZimGlobalState &g, const ReadZimBindData &bind) {
	g.file_idx++;
	if (g.file_idx < g.files.size()) {
		OpenCurrentFile(g, bind);
	} else {
		g.archive.reset();
		g.cursor.reset();
	}
}

unique_ptr<GlobalTableFunctionState> ReadZimInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadZimBindData>();
	auto state = make_uniq<ReadZimGlobalState>();
	state->files = bind.file_paths;
	state->column_ids = input.column_ids;

	// Projection pushdown: only load blobs if the content column is actually projected
	// AND the caller opted in via include_content. SELECT * without include_content
	// therefore does NOT decompress the archive.
	bool content_projected = false;
	for (auto cid : input.column_ids) {
		if (cid == COL_CONTENT) {
			content_projected = true;
		}
	}
	state->want_content = content_projected && bind.include_content;

	OpenCurrentFile(*state, bind); // open the first archive (file_paths is non-empty)
	return std::move(state);
}

void EmitRow(const ReadZimBindData &bind, const ReadZimGlobalState &gstate, const ZimEntry &row, DataChunk &output,
             idx_t out_idx) {
	for (idx_t col = 0; col < gstate.column_ids.size(); col++) {
		auto cid = gstate.column_ids[col];
		auto &vec = output.data[col];
		switch (cid) {
		case COL_PATH:
			vec.SetValue(out_idx, Value(row.path));
			break;
		case COL_TITLE:
			vec.SetValue(out_idx, Value(row.title));
			break;
		case COL_MIMETYPE:
			vec.SetValue(out_idx, row.is_redirect ? Value(LogicalType::VARCHAR) : Value(row.mimetype));
			break;
		case COL_IS_REDIRECT:
			vec.SetValue(out_idx, Value::BOOLEAN(row.is_redirect));
			break;
		case COL_REDIRECT_PATH:
			vec.SetValue(out_idx, row.is_redirect ? Value(row.redirect_path) : Value(LogicalType::VARCHAR));
			break;
		case COL_SIZE:
			vec.SetValue(out_idx, row.is_redirect ? Value(LogicalType::UBIGINT) : Value::UBIGINT(row.size));
			break;
		case COL_CONTENT: {
			if (!gstate.want_content || row.is_redirect) {
				vec.SetValue(out_idx, Value(ContentType(bind)));
			} else if (bind.content_as_varchar) {
				// Binary content -> NULL rather than invalid UTF-8 in a VARCHAR column.
				vec.SetValue(out_idx, IsValidUtf8(row.content) ? Value(row.content) : Value(LogicalType::VARCHAR));
			} else {
				vec.SetValue(out_idx, Value::BLOB_RAW(row.content));
			}
			break;
		}
		case COL_FILE_PATH:
			// The archive this row came from (file_idx is unchanged until after emit).
			vec.SetValue(out_idx, Value(gstate.files[gstate.file_idx]));
			break;
		default:
			break;
		}
	}
}

void ReadZimFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<ReadZimBindData>();
	auto &gstate = data.global_state->Cast<ReadZimGlobalState>();

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.file_idx < gstate.files.size()) {
		if (bind.single_lookup) {
			// Per-file exact lookup: emit at most one row for the current archive, then
			// move on. An exact path present in N archives therefore emits N rows.
			if (!gstate.lookup_emitted) {
				gstate.lookup_emitted = true;
				auto row = bind.lookup_by_title ? gstate.archive->GetByTitle(bind.lookup_key, gstate.want_content)
				                                : gstate.archive->GetByPath(bind.lookup_key, gstate.want_content);
				if (row.has_value() && (!bind.spec.mimetype.has_value() || row->mimetype == *bind.spec.mimetype)) {
					EmitRow(bind, gstate, *row, output, count);
					count++;
				}
			}
			AdvanceFile(gstate, bind);
		} else {
			// Scan mode: drain the current cursor, then advance to the next archive.
			// Critically, a finished cursor advances files rather than ending the scan,
			// so later files in a glob/list are not silently dropped.
			ZimEntry row;
			if (gstate.cursor->Next(row)) {
				EmitRow(bind, gstate, row, output, count);
				count++;
			} else {
				AdvanceFile(gstate, bind);
			}
		}
	}
	output.SetCardinality(count);
}

// Rewrites `FROM 'archive.zim'` into read_zim('archive.zim').
static unique_ptr<TableRef> ReadZimReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                                   optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"zim"})) {
		return nullptr; // not a .zim name: let normal name resolution / errors proceed
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("read_zim", std::move(children));
	return std::move(table_function);
}

} // namespace

void RegisterReadZim(ExtensionLoader &loader) {
	auto make_fn = [](const LogicalType &arg_type) {
		TableFunction f("read_zim", {arg_type}, ReadZimFunction, ReadZimBind, ReadZimInitGlobal);
		f.named_parameters["include_content"] = LogicalType::BOOLEAN;
		f.named_parameters["content_as_varchar"] = LogicalType::BOOLEAN;
		f.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
		f.named_parameters["filename"] = LogicalType::BOOLEAN;
		f.named_parameters["mimetype"] = LogicalType::VARCHAR;
		f.named_parameters["path"] = LogicalType::VARCHAR;
		f.named_parameters["title"] = LogicalType::VARCHAR;
		f.named_parameters["path_prefix"] = LogicalType::VARCHAR;
		f.named_parameters["title_prefix"] = LogicalType::VARCHAR;
		f.named_parameters["listing"] = LogicalType::VARCHAR;
		f.projection_pushdown = true;
		return f;
	};

	// Two arities: a single VARCHAR (path or glob) and a LIST(VARCHAR) of them.
	TableFunctionSet set("read_zim");
	set.AddFunction(make_fn(LogicalType::VARCHAR));
	set.AddFunction(make_fn(LogicalType::LIST(LogicalType::VARCHAR)));
	loader.RegisterFunction(set);

	// `FROM 'x.zim'` replacement scan (only fires for names ending in .zim).
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(ReadZimReplacementScan);
}

} // namespace duckdb
