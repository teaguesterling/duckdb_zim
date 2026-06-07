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
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

#include <memory>

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
	std::string file_path;
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
	std::shared_ptr<ZimArchive> archive;
	std::unique_ptr<ZimScanCursor> cursor; // null in single_lookup mode
	bool single_done = false;              // single_lookup: have we emitted?
	std::vector<column_t> column_ids;      // projection
	bool want_content = false;             // content projected AND include_content
	idx_t MaxThreads() const override { return 1; } // single-archive sequential scan
};

LogicalType ContentType(const ReadZimBindData &bind) {
	return bind.content_as_varchar ? LogicalType::VARCHAR : LogicalType::BLOB;
}

unique_ptr<FunctionData> ReadZimBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadZimBindData>();
	bind->file_path = input.inputs[0].GetValue<string>();

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
			bind->single_lookup = true;
			bind->lookup_by_title = false;
			bind->lookup_key = val.GetValue<string>();
		} else if (key == "title") {
			bind->single_lookup = true;
			bind->lookup_by_title = true;
			bind->lookup_key = val.GetValue<string>();
		} else if (key == "listing") {
			// Picks which libzim listing to traverse, not merely an ordering:
			// 'path' = all user entries (path order); 'title' = the front-article
			// title listing (iterByTitle yields only FRONT_ARTICLE entries).
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

	// A title_prefix implies title order; a path_prefix implies path order.
	if (bind->spec.title_prefix.has_value()) {
		bind->spec.order = ScanOrder::ByTitle;
	} else if (bind->spec.path_prefix.has_value()) {
		bind->spec.order = ScanOrder::ByPath;
	}

	names = {"path", "title", "mimetype", "is_redirect", "redirect_path", "size", "content"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::UBIGINT,
	                ContentType(*bind)};
	if (bind->include_filepath) {
		names.emplace_back("file_path");
		return_types.emplace_back(LogicalType::VARCHAR);
		bind->column_count = COL_COUNT;
	} else {
		bind->column_count = COL_FILE_PATH;
	}
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> ReadZimInitGlobal(ClientContext &context,
                                                       TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadZimBindData>();
	auto state = make_uniq<ReadZimGlobalState>();

	state->archive = ArchivePool::Instance().Get(bind.file_path);
	state->column_ids = input.column_ids;

	// Projection pushdown: only load blobs if the content column is actually
	// projected AND the caller opted in via include_content. SELECT * without
	// include_content therefore does NOT decompress the archive.
	bool content_projected = false;
	for (auto cid : input.column_ids) {
		if (cid == COL_CONTENT) {
			content_projected = true;
		}
	}
	state->want_content = content_projected && bind.include_content;

	if (!bind.single_lookup) {
		ScanSpec spec = bind.spec;
		spec.want_content = state->want_content;
		state->cursor = make_uniq<ZimScanCursor>(state->archive->Scan(spec));
	}
	return std::move(state);
}

void EmitRow(const ReadZimBindData &bind, const ReadZimGlobalState &gstate, const ZimEntry &row,
             DataChunk &output, idx_t out_idx) {
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
			vec.SetValue(out_idx, row.is_redirect ? Value(LogicalType::VARCHAR)
			                                      : Value(row.mimetype));
			break;
		case COL_IS_REDIRECT:
			vec.SetValue(out_idx, Value::BOOLEAN(row.is_redirect));
			break;
		case COL_REDIRECT_PATH:
			vec.SetValue(out_idx, row.is_redirect ? Value(row.redirect_path)
			                                      : Value(LogicalType::VARCHAR));
			break;
		case COL_SIZE:
			vec.SetValue(out_idx, row.is_redirect ? Value(LogicalType::UBIGINT)
			                                      : Value::UBIGINT(row.size));
			break;
		case COL_CONTENT: {
			if (!gstate.want_content || row.is_redirect) {
				vec.SetValue(out_idx, Value(ContentType(bind)));
			} else if (bind.content_as_varchar) {
				// Binary content -> NULL rather than invalid UTF-8 in a VARCHAR column.
				vec.SetValue(out_idx, IsValidUtf8(row.content) ? Value(row.content)
				                                               : Value(LogicalType::VARCHAR));
			} else {
				vec.SetValue(out_idx, Value::BLOB_RAW(row.content));
			}
			break;
		}
		case COL_FILE_PATH:
			vec.SetValue(out_idx, Value(bind.file_path));
			break;
		default:
			break;
		}
	}
}

void ReadZimFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<ReadZimBindData>();
	auto &gstate = data.global_state->Cast<ReadZimGlobalState>();

	if (bind.single_lookup) {
		if (gstate.single_done) {
			output.SetCardinality(0);
			return;
		}
		gstate.single_done = true;
		std::optional<ZimEntry> row =
		    bind.lookup_by_title ? gstate.archive->GetByTitle(bind.lookup_key, gstate.want_content)
		                         : gstate.archive->GetByPath(bind.lookup_key, gstate.want_content);
		if (!row.has_value()) {
			output.SetCardinality(0);
			return;
		}
		if (bind.spec.mimetype.has_value() && row->mimetype != *bind.spec.mimetype) {
			output.SetCardinality(0);
			return;
		}
		EmitRow(bind, gstate, *row, output, 0);
		output.SetCardinality(1);
		return;
	}

	idx_t count = 0;
	ZimEntry row;
	while (count < STANDARD_VECTOR_SIZE && gstate.cursor->Next(row)) {
		EmitRow(bind, gstate, row, output, count);
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterReadZim(ExtensionLoader &loader) {
	TableFunction read_zim("read_zim", {LogicalType::VARCHAR}, ReadZimFunction, ReadZimBind,
	                       ReadZimInitGlobal);
	read_zim.named_parameters["include_content"] = LogicalType::BOOLEAN;
	read_zim.named_parameters["content_as_varchar"] = LogicalType::BOOLEAN;
	read_zim.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
	read_zim.named_parameters["filename"] = LogicalType::BOOLEAN;
	read_zim.named_parameters["mimetype"] = LogicalType::VARCHAR;
	read_zim.named_parameters["path"] = LogicalType::VARCHAR;
	read_zim.named_parameters["title"] = LogicalType::VARCHAR;
	read_zim.named_parameters["path_prefix"] = LogicalType::VARCHAR;
	read_zim.named_parameters["title_prefix"] = LogicalType::VARCHAR;
	read_zim.named_parameters["listing"] = LogicalType::VARCHAR;
	read_zim.projection_pushdown = true;
	loader.RegisterFunction(read_zim);

	// TODO(phase1+): replacement scan for `FROM 'x.zim'` and multi-file/glob
	// inputs (LogicalType::LIST(VARCHAR)) to match read_markdown ergonomics.
}

} // namespace duckdb
