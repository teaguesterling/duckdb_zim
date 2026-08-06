//===----------------------------------------------------------------------===//
// zim_search.cpp — full-text search + title suggestions over ZIM archives.
//
//   zim_search(files, query [, max_results := 25, result_offset := 0, with_snippet := true])
//       -> table(path, title, score DOUBLE, snippet VARCHAR, file VARCHAR)
//   zim_suggest(files, query [, max_results := 25, result_offset := 0, with_snippet := true])
//       -> table(path, title, snippet VARCHAR, file VARCHAR)
//
// `files` is a single path, a glob, or a LIST(VARCHAR) — like read_zim — so a
// query can run across many archives at once (federated search). max_results /
// result_offset apply PER archive (each contributes up to max_results hits);
// the trailing `file` column says which archive a hit came from, and you ORDER
// BY / LIMIT across them in SQL. Xapian ranks are per-archive and not directly
// comparable across files.
//
// Search is backed by libzim's Searcher (Xapian); the access layer's Search()
// is guarded by LIBZIM_WITH_XAPIAN and yields nothing on a search-less build
// (e.g. Wasm). Suggest degrades to a title-prefix listing there instead.
//
// `limit` / `offset` are SQL reserved words and won't parse as bare named
// parameters (same reason phase 1 used `listing`, not `order`), hence
// `max_results` / `result_offset`.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

namespace duckdb {

using zim_ext::GetArchivePool;
using zim_ext::ZimArchive;
using zim_ext::ZimSearchHit;

namespace {

struct SearchBindData : public TableFunctionData {
	std::vector<std::string> file_paths; // one or more archives (glob/list expanded)
	std::string query;
	uint32_t max_results = 25;
	uint32_t result_offset = 0;
	bool with_snippet = true;
	// When true, a federated call (>1 archive) tolerates archives it can't open:
	// as long as at least one archive succeeds, the un-openable ones are skipped
	// (logged) instead of aborting the whole query. A single archive, or an
	// all-archives-failed federation, still throws. Default false (abort).
	bool ignore_errors = false;
};

// A hit plus the archive it came from.
struct TaggedHit {
	std::string file;
	ZimSearchHit hit;
};

struct SearchGlobalState : public GlobalTableFunctionState {
	std::vector<TaggedHit> hits;
	idx_t pos = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static uint32_t NonNegativeParam(const Value &val, const char *name) {
	// GetValue on a NULL Value raises an INTERNAL assertion; reject it as a
	// clean binder error instead. A NULL here typically arrives when a caller
	// passes an omitted optional argument straight through (a prepared-statement
	// parameter, or an MCP tool parameter the caller left out).
	if (val.IsNull()) {
		throw BinderException("%s must not be NULL", name);
	}
	auto v = val.GetValue<int64_t>();
	if (v < 0) {
		throw BinderException("%s must be >= 0", name);
	}
	return static_cast<uint32_t>(v);
}

// A BOOLEAN named parameter, rejecting NULL for the same reason as
// NonNegativeParam (GetValue<bool> on a NULL Value trips an internal assertion).
static bool BoolParam(const Value &val, const char *name) {
	if (val.IsNull()) {
		throw BinderException("%s must not be NULL", name);
	}
	return val.GetValue<bool>();
}

// Expand a VARCHAR / glob / LIST(VARCHAR) argument into a list of archive paths.
static std::vector<std::string> ExpandFiles(ClientContext &context, const Value &arg, const char *fn) {
	std::vector<std::string> patterns;
	if (arg.type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(arg)) {
			patterns.push_back(child.GetValue<std::string>());
		}
	} else {
		patterns.push_back(arg.GetValue<std::string>());
	}
	auto &fs = FileSystem::GetFileSystem(context);
	std::vector<std::string> files;
	for (auto &pat : patterns) {
		if (FileSystem::HasGlob(pat)) {
			for (auto &info : fs.Glob(pat)) {
				files.push_back(info.path);
			}
		} else {
			files.push_back(pat);
		}
	}
	if (files.empty()) {
		throw BinderException("%s: no files matched the given pattern(s)", fn);
	}
	return files;
}

static void ParseSearchParams(SearchBindData &bind, TableFunctionBindInput &input, const char *fn) {
	for (auto &kv : input.named_parameters) {
		if (kv.first == "max_results") {
			bind.max_results = NonNegativeParam(kv.second, "max_results");
		} else if (kv.first == "result_offset") {
			bind.result_offset = NonNegativeParam(kv.second, "result_offset");
		} else if (kv.first == "with_snippet") {
			bind.with_snippet = BoolParam(kv.second, "with_snippet");
		} else if (kv.first == "ignore_errors") {
			bind.ignore_errors = BoolParam(kv.second, "ignore_errors");
		} else {
			throw BinderException("%s: unknown parameter '%s'", fn, kv.first);
		}
	}
}

unique_ptr<FunctionData> SearchBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<SearchBindData>();
	bind->file_paths = ExpandFiles(context, input.inputs[0], "zim_search");
	bind->query = input.inputs[1].GetValue<string>();
	ParseSearchParams(*bind, input, "zim_search");
	names = {"path", "title", "score", "snippet", "file"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	return std::move(bind);
}

// Resolve the remote-search local-index cap from the setting (falls back to the default).
static uint64_t ResolveMaxLocalIndex(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("zim_remote_search_max_local_index", v) && !v.IsNull()) {
		return v.GetValue<uint64_t>();
	}
	return zim_ext::DEFAULT_MAX_LOCAL_SEARCH_INDEX;
}

// Resolve the decompression-bomb output cap (bounds per-hit snippet body reads).
static uint64_t ResolveMaxContentSize(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("zim_max_content_size", v) && !v.IsNull()) {
		return v.GetValue<uint64_t>();
	}
	return zim_ext::DEFAULT_MAX_CONTENT_SIZE;
}

// Run `fetch` against each archive in bind.file_paths, tagging every hit with its
// source file, and apply the ignore_errors policy:
//   - Default (ignore_errors=false): any failure aborts the whole call.
//   - ignore_errors=true: an archive that can't be opened/searched is skipped
//     (logged), BUT only when it's a multi-archive call and at least one archive
//     still succeeds. A single archive, or a federation where every archive fails,
//     re-throws -- ignore_errors salvages a partial result, it never hides a total
//     failure or a lone bad target.
template <typename FetchFn>
static std::vector<TaggedHit> RunFederated(ClientContext &context, const SearchBindData &bind, const char *fn,
                                           FetchFn fetch) {
	auto &pool = GetArchivePool(context);
	auto *fs = &FileSystem::GetFileSystem(context);
	const uint64_t max_local_index = ResolveMaxLocalIndex(context);

	std::vector<TaggedHit> hits;
	const size_t total = bind.file_paths.size();
	size_t failures = 0;
	std::string failure_notes;
	std::exception_ptr first_error;

	for (auto &fp : bind.file_paths) {
		try {
			auto archive = pool.Get(fp, fs, max_local_index);
			for (auto &hit : fetch(*archive)) {
				hits.push_back(TaggedHit {fp, std::move(hit)});
			}
		} catch (...) {
			if (!bind.ignore_errors) {
				throw; // default: abort on the first un-openable archive
			}
			++failures;
			if (!first_error) {
				first_error = std::current_exception();
			}
			std::string msg;
			try {
				std::rethrow_exception(std::current_exception());
			} catch (const std::exception &e) {
				msg = e.what();
			} catch (...) {
				msg = "unknown error";
			}
			if (!failure_notes.empty()) {
				failure_notes += "\n";
			}
			failure_notes += fp + ": " + msg;
			DUCKDB_LOG_WARNING(context, "%s: skipped un-openable archive '%s': %s", fn, fp, msg);
			std::fprintf(stderr, "[zim] %s: skipped un-openable archive '%s': %s\n", fn, fp.c_str(), msg.c_str());
		}
	}

	// ignore_errors tolerates a partial federation only; surface the error when a
	// lone archive fails or every archive fails (nothing salvageable).
	if (bind.ignore_errors && failures > 0 && (total <= 1 || failures == total)) {
		if (total == 1 && first_error) {
			std::rethrow_exception(first_error); // preserve the exact single-archive error
		}
		throw IOException("%s: all %s archive(s) failed:\n%s", fn, std::to_string(total), failure_notes);
	}
	return hits;
}

unique_ptr<GlobalTableFunctionState> SearchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<SearchBindData>();
	auto state = make_uniq<SearchGlobalState>();
	const uint64_t max_content = ResolveMaxContentSize(context);
	state->hits = RunFederated(context, bind, "zim_search", [&](ZimArchive &archive) {
		return archive.Search(bind.query, bind.result_offset, bind.max_results, bind.with_snippet, max_content);
	});
	return std::move(state);
}

void SearchFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<SearchGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.pos < gstate.hits.size()) {
		const auto &tagged = gstate.hits[gstate.pos++];
		const auto &hit = tagged.hit;
		output.data[0].SetValue(count, Value(hit.path));
		output.data[1].SetValue(count, Value(hit.title));
		// score / snippet are optional: NULL when the pinned libzim's iterator
		// doesn't expose them.
		output.data[2].SetValue(count, hit.score.has_value() ? Value::DOUBLE(*hit.score) : Value(LogicalType::DOUBLE));
		output.data[3].SetValue(count, hit.snippet.has_value() ? Value(*hit.snippet) : Value(LogicalType::VARCHAR));
		output.data[4].SetValue(count, Value(tagged.file));
		count++;
	}
	output.SetCardinality(count);
}

// --- zim_suggest: title autocomplete (reuses SearchBindData / SearchGlobalState) ---

unique_ptr<FunctionData> SuggestBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<SearchBindData>();
	bind->file_paths = ExpandFiles(context, input.inputs[0], "zim_suggest");
	bind->query = input.inputs[1].GetValue<string>();
	ParseSearchParams(*bind, input, "zim_suggest");
	// Suggestions have no fulltext score; just path/title/snippet (+ source file).
	names = {"path", "title", "snippet", "file"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> SuggestInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<SearchBindData>();
	auto state = make_uniq<SearchGlobalState>();
	state->hits = RunFederated(context, bind, "zim_suggest", [&](ZimArchive &archive) {
		return archive.Suggest(bind.query, bind.result_offset, bind.max_results, bind.with_snippet);
	});
	return std::move(state);
}

void SuggestFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<SearchGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.pos < gstate.hits.size()) {
		const auto &tagged = gstate.hits[gstate.pos++];
		const auto &hit = tagged.hit;
		output.data[0].SetValue(count, Value(hit.path));
		output.data[1].SetValue(count, Value(hit.title));
		output.data[2].SetValue(count, hit.snippet.has_value() ? Value(*hit.snippet) : Value(LogicalType::VARCHAR));
		output.data[3].SetValue(count, Value(tagged.file));
		count++;
	}
	output.SetCardinality(count);
}

TableFunction MakeFn(const string &name, const LogicalType &files_type, table_function_t fn, table_function_bind_t bind,
                     table_function_init_global_t init) {
	TableFunction f(name, {files_type, LogicalType::VARCHAR}, fn, bind, init);
	f.named_parameters["max_results"] = LogicalType::BIGINT;
	f.named_parameters["result_offset"] = LogicalType::BIGINT;
	f.named_parameters["with_snippet"] = LogicalType::BOOLEAN;
	f.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
	return f;
}

} // namespace

void RegisterZimSearch(ExtensionLoader &loader) {
	const auto LIST_V = LogicalType::LIST(LogicalType::VARCHAR);

	TableFunctionSet search("zim_search");
	search.AddFunction(MakeFn("zim_search", LogicalType::VARCHAR, SearchFunction, SearchBind, SearchInit));
	search.AddFunction(MakeFn("zim_search", LIST_V, SearchFunction, SearchBind, SearchInit));
	loader.RegisterFunction(search);

	TableFunctionSet suggest("zim_suggest");
	suggest.AddFunction(MakeFn("zim_suggest", LogicalType::VARCHAR, SuggestFunction, SuggestBind, SuggestInit));
	suggest.AddFunction(MakeFn("zim_suggest", LIST_V, SuggestFunction, SuggestBind, SuggestInit));
	loader.RegisterFunction(suggest);
}

} // namespace duckdb
