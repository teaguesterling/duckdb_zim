//===----------------------------------------------------------------------===//
// zim_search.cpp — full-text search over a ZIM's Xapian index (DESIGN §4.4).
//
//   zim_search(file, query [, max_results := 25, result_offset := 0])
//       -> table(path, title, score DOUBLE, snippet VARCHAR)
//
// Backed by libzim's Searcher (Xapian). This translation unit is only useful
// when libzim was built with xapian; the access layer's Search() is itself
// guarded by LIBZIM_WITH_XAPIAN and returns no hits otherwise, so on a
// search-less build (e.g. Wasm) this function binds but always yields 0 rows.
//
// `limit` / `offset` are SQL reserved words and won't parse as bare named
// parameters (same reason phase 1 used `listing`, not `order`), hence
// `max_results` / `result_offset`.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

namespace duckdb {

using zim_ext::ArchivePool;
using zim_ext::ZimArchive;
using zim_ext::ZimSearchHit;

namespace {

struct SearchBindData : public TableFunctionData {
	std::string file_path;
	std::string query;
	uint32_t max_results = 25;
	uint32_t result_offset = 0;
};

struct SearchGlobalState : public GlobalTableFunctionState {
	std::vector<ZimSearchHit> hits;
	idx_t pos = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static uint32_t NonNegativeParam(const Value &val, const char *name) {
	auto v = val.GetValue<int64_t>();
	if (v < 0) {
		throw BinderException("zim_search: %s must be >= 0", name);
	}
	return static_cast<uint32_t>(v);
}

unique_ptr<FunctionData> SearchBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	auto bind = make_uniq<SearchBindData>();
	bind->file_path = input.inputs[0].GetValue<string>();
	bind->query = input.inputs[1].GetValue<string>();
	for (auto &kv : input.named_parameters) {
		if (kv.first == "max_results") {
			bind->max_results = NonNegativeParam(kv.second, "max_results");
		} else if (kv.first == "result_offset") {
			bind->result_offset = NonNegativeParam(kv.second, "result_offset");
		} else {
			throw BinderException("zim_search: unknown parameter '%s'", kv.first);
		}
	}
	names = {"path", "title", "score", "snippet"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::VARCHAR};
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> SearchInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<SearchBindData>();
	auto state = make_uniq<SearchGlobalState>();
	auto archive = ArchivePool::Instance().Get(bind.file_path);
	// No fulltext index (or a search-less libzim build) -> empty result set.
	state->hits = archive->Search(bind.query, bind.result_offset, bind.max_results);
	return std::move(state);
}

void SearchFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<SearchGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.pos < gstate.hits.size()) {
		const auto &hit = gstate.hits[gstate.pos++];
		output.data[0].SetValue(count, Value(hit.path));
		output.data[1].SetValue(count, Value(hit.title));
		// score / snippet are optional: NULL when the pinned libzim's search
		// iterator doesn't expose them.
		output.data[2].SetValue(count, hit.score.has_value() ? Value::DOUBLE(*hit.score) : Value(LogicalType::DOUBLE));
		output.data[3].SetValue(count, hit.snippet.has_value() ? Value(*hit.snippet) : Value(LogicalType::VARCHAR));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterZimSearch(ExtensionLoader &loader) {
	TableFunction search("zim_search", {LogicalType::VARCHAR, LogicalType::VARCHAR}, SearchFunction, SearchBind,
	                     SearchInit);
	search.named_parameters["max_results"] = LogicalType::BIGINT;
	search.named_parameters["result_offset"] = LogicalType::BIGINT;
	loader.RegisterFunction(search);
}

} // namespace duckdb
