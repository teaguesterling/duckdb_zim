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
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace duckdb {

using zim_ext::ArchivePool;
using zim_ext::GetArchivePool;
using zim_ext::MimetypeAccepted;
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

// Resolve the decompression-bomb output cap from the setting (falls back to the default).
static uint64_t ResolveMaxContentSize(ClientContext &context) {
	Value v;
	if (context.TryGetCurrentSetting("zim_max_content_size", v) && !v.IsNull()) {
		return v.GetValue<uint64_t>();
	}
	return zim_ext::DEFAULT_MAX_CONTENT_SIZE;
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

// How the scan executes. ParallelScan partitions the cluster-order index space
// across DuckDB threads; the others are single-threaded.
//   * Lookup       — exact path/title (0/1 row per archive)
//   * SerialScan   — cursor over a prefix range or the title listing (order matters)
//   * ParallelScan — the plain path-order full scan (order is not guaranteed)
enum class ScanMode { Lookup, SerialScan, ParallelScan };

struct ReadZimBindData : public TableFunctionData {
	std::vector<std::string> file_paths; // one or more archives (glob/list expanded)
	// resolved options
	bool include_content = false;
	bool content_as_varchar = false;
	bool include_filepath = false;
	bool parallel = true; // opt out with parallel := false (forces SerialScan)
	ScanSpec spec;
	// exact-lookup mode (path:= or title:=) emits 0/1 row instead of a scan
	bool single_lookup = false;
	bool lookup_by_title = false;
	std::string lookup_key;
	ScanMode mode = ScanMode::SerialScan; // resolved at the end of bind
	idx_t column_count = COL_FILE_PATH;   // becomes COL_COUNT if include_filepath
};

struct ReadZimGlobalState : public GlobalTableFunctionState {
	ScanMode mode = ScanMode::SerialScan;
	std::vector<std::string> files;   // archives to scan, in order
	std::vector<column_t> column_ids; // projection
	bool want_content = false;        // content projected AND include_content
	ScanSpec scan_spec;               // bind.spec + want_content; shared by cursor and ScanIndex
	idx_t max_threads = 1;
	FileSystem *fs = nullptr;    // for remote (s3/http) opens; set at InitGlobal from the context
	ArchivePool *pool = nullptr; // per-DB pool from the context's ObjectCache; set at InitGlobal

	// Exact-lookup parameters. Seeded from bind (path:=/title:=) and possibly set by
	// a pushed-down `WHERE path = const` / `WHERE title = const` at init.
	bool single_lookup = false;
	bool lookup_by_title = false;
	std::string lookup_key;

	// Every filter DuckDB pushed down, rebuilt as one AND-ed expression over the output
	// chunk's layout (BoundReference i == output column i == column_ids[i]). DuckDB
	// DELETES a fully-pushed filter from the plan, so the scan must apply it itself —
	// see the filter-pushdown note below. Null when nothing was pushed.
	unique_ptr<Expression> filter_expression;

	// --- serial (Lookup / SerialScan): sequential file/cursor state machine ---
	idx_t file_idx = 0;
	std::shared_ptr<ZimArchive> archive;
	std::unique_ptr<ZimScanCursor> cursor;
	bool lookup_emitted = false;

	// --- parallel (ParallelScan): cluster-order morsel dispenser ---
	std::mutex morsel_lock;
	idx_t p_file = 0;
	uint64_t p_index = 0;
	uint64_t p_count = 0;
	std::shared_ptr<ZimArchive> p_archive;

	// Hands the next [start, end) cluster-order morsel (within one archive) to a
	// thread. Returns false once every file is fully dispensed. Thread-safe; the
	// pooled libzim Archive is itself threadsafe, so the morsels read concurrently.
	bool NextMorsel(idx_t &out_file, uint64_t &out_start, uint64_t &out_end, std::shared_ptr<ZimArchive> &out_archive) {
		static constexpr uint64_t MORSEL = 4096;
		std::lock_guard<std::mutex> guard(morsel_lock);
		while (p_file < files.size()) {
			if (!p_archive) {
				p_archive = pool->Get(files[p_file], fs); // may throw
				p_count = p_archive->ContentEntryCount();
				p_index = 0;
			}
			if (p_index < p_count) {
				out_file = p_file;
				out_start = p_index;
				out_end = std::min(p_index + MORSEL, p_count);
				out_archive = p_archive;
				p_index = out_end;
				return true;
			}
			p_file++;
			p_archive.reset();
		}
		return false;
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct ReadZimLocalState : public LocalTableFunctionState {
	std::shared_ptr<ZimArchive> archive; // current morsel's archive
	idx_t file_idx = 0;
	uint64_t idx = 0; // next cluster-order index to read
	uint64_t end = 0; // end of the current morsel

	// --- pushed-down filter application (only allocated when filters were pushed) ---
	// Rows are emitted into scan_chunk and then selected into the caller's chunk, so
	// EmitRow always writes to flat vectors regardless of how the last chunk was sliced.
	DataChunk scan_chunk;
	unique_ptr<ExpressionExecutor> filter_executor;
	SelectionVector filter_sel;
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
			// Accept BOOLEAN (all / none), a single VARCHAR mimetype, or a
			// LIST(VARCHAR) of mimetypes. A mimetype form turns content loading on
			// but gates it: only matching entries are decompressed; everything else
			// comes back with NULL content (filter pushdown, not post-filtering).
			auto type_id = val.type().id();
			if (type_id == LogicalTypeId::BOOLEAN) {
				bind->include_content = BooleanValue::Get(val);
			} else if (type_id == LogicalTypeId::VARCHAR) {
				bind->include_content = true;
				bind->spec.content_mimetypes.push_back(val.GetValue<string>());
			} else if (type_id == LogicalTypeId::LIST) {
				for (auto &child : ListValue::GetChildren(val)) {
					bind->spec.content_mimetypes.push_back(child.GetValue<string>());
				}
				// An empty list selects zero mimetypes -> load no content (an empty
				// gate otherwise means "all", which is the BOOLEAN-true semantics).
				bind->include_content = !bind->spec.content_mimetypes.empty();
			} else {
				throw BinderException("read_zim: include_content must be a BOOLEAN, a VARCHAR mimetype, "
				                      "or a LIST of mimetypes");
			}
		} else if (key == "content_as_varchar") {
			bind->content_as_varchar = BooleanValue::Get(val);
		} else if (key == "parallel") {
			bind->parallel = BooleanValue::Get(val);
		} else if (key == "include_filepath" || key == "filename") {
			bind->include_filepath = BooleanValue::Get(val);
		} else if (key == "mimetype") {
			// Accept-style row filter: a single mimetype, or a LIST, with wildcard
			// patterns (image/*, */*, *). Empty list = no filter.
			auto type_id = val.type().id();
			if (type_id == LogicalTypeId::LIST) {
				for (auto &child : ListValue::GetChildren(val)) {
					bind->spec.mimetype_filter.push_back(child.GetValue<string>());
				}
			} else {
				bind->spec.mimetype_filter.push_back(val.GetValue<string>());
			}
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

	// Resolve execution mode. Only the plain path-order full scan parallelizes (it
	// has no ordering or range contract); exact lookups, prefix ranges, and the title
	// listing stay single-threaded, as does an explicit `parallel := false`.
	if (bind->single_lookup) {
		bind->mode = ScanMode::Lookup;
	} else if (bind->parallel && !has_path_prefix && !has_title_prefix && bind->spec.order == ScanOrder::ByPath) {
		bind->mode = ScanMode::ParallelScan;
	} else {
		bind->mode = ScanMode::SerialScan;
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
// Serial paths only (Lookup / SerialScan). May throw — surfaced as the query's error.
static void OpenCurrentFile(ReadZimGlobalState &g, const ReadZimBindData &bind) {
	g.archive = g.pool->Get(g.files[g.file_idx], g.fs); // may throw
	g.lookup_emitted = false;
	if (!g.single_lookup) {
		g.cursor = make_uniq<ZimScanCursor>(g.archive->Scan(g.scan_spec));
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

// --- filter pushdown -------------------------------------------------------
// filter_pushdown = true is NOT advisory. Once a filter lands in the scan's
// TableFilterSet, DuckDB removes it from the plan and never re-applies it:
//   * FilterCombiner::TryPushdownConstantFilter ends with equivalence_map.erase(),
//     so `size > N` / `mimetype = 'x'` are not regenerated above the GET;
//   * GenerateTableScanFilters erases any filter TryPushdownExpression reports as
//     PUSHED_DOWN_FULLY — `col LIKE 'x%'` becomes `col >= 'x' AND col < 'y'`,
//     `col IN (a,b,c)` on consecutive integers becomes a range, etc.
// Anything the scan silently ignores is therefore silently *dropped* — the cause
// of issue #29 (WHERE mimetype LIKE 'image/%' returning the whole archive,
// NULL mimetypes included).
//
// So the scan applies the pushed filters itself, in two layers:
//
//  1. Read reduction (best-effort, must stay a SUPERSET of the true result):
//     `path = const` / `title = const` -> exact libzim lookup (the big win,
//     especially remote — a few KB instead of a full dirent scan); `mimetype =
//     const` / `mimetype IN (...)` -> the Accept-style scan filter.
//  2. Correctness (exhaustive): BuildFilterExpression turns the ENTIRE
//     TableFilterSet back into a bound expression via TableFilter::ToExpression
//     and ReadZimFunction evaluates it over every chunk. Layer 1 may therefore
//     handle nothing at all without affecting the result set.

// Output type of a storage column, for the BoundReferenceExpression the filters bind to.
static LogicalType ZimColumnType(const ReadZimBindData &bind, column_t cid) {
	switch (cid) {
	case COL_PATH:
	case COL_TITLE:
	case COL_MIMETYPE:
	case COL_REDIRECT_PATH:
	case COL_FILE_PATH:
		return LogicalType::VARCHAR;
	case COL_IS_REDIRECT:
		return LogicalType::BOOLEAN;
	case COL_SIZE:
		return LogicalType::UBIGINT;
	case COL_CONTENT:
		return ContentType(bind);
	default:
		return LogicalType::INVALID;
	}
}

// Rebuild the pushed-down TableFilterSet as a single AND-ed expression over the output
// chunk. Filters are keyed by *projected* index (PhysicalPlanGenerator's
// CreateTableFilterSet rewrites the storage index into an index into column_ids), which
// is exactly the output chunk's column order — filter_prune is off, so no projection_ids
// indirection sits in between.
//
// A filter we cannot map raises rather than being skipped: skipping is what made issue
// #29 silent, and this layer is the one results depend on. The type is
// NotImplementedException, not InternalException (issue #35): DuckDB invalidates the
// whole DatabaseInstance on ExceptionType::INTERNAL, so an unmapped column would cost the
// user their connection instead of failing one query. Neither guard is reachable from SQL
// today — every projected column id comes from the bound column list and read_zim exposes
// no rowid — they exist to catch a future virtual column or column_t sentinel, and that is
// exactly a "not implemented", not a broken invariant. ApplyPushedFilters skips the same
// condition instead of raising because it only reads less; the error still surfaces here,
// a few lines later, so nothing is silently dropped either way.
static unique_ptr<Expression> BuildFilterExpression(const ReadZimBindData &bind, const ReadZimGlobalState &g,
                                                    TableFunctionInitInput &input) {
	if (!input.filters || input.filters->filters.empty()) {
		return nullptr;
	}
	vector<unique_ptr<Expression>> conditions;
	for (auto &entry : input.filters->filters) {
		const idx_t proj_idx = entry.first;
		if (proj_idx >= g.column_ids.size()) {
			throw NotImplementedException("read_zim: pushed-down filter references column %llu, but only %llu columns "
			                              "are projected",
			                              (uint64_t)proj_idx, (uint64_t)g.column_ids.size());
		}
		auto type = ZimColumnType(bind, g.column_ids[proj_idx]);
		if (type.id() == LogicalTypeId::INVALID) {
			throw NotImplementedException("read_zim: pushed-down filter on unknown column id %llu",
			                              (uint64_t)g.column_ids[proj_idx]);
		}
		auto col_ref = make_uniq<BoundReferenceExpression>(type, proj_idx);
		// ToExpression is total over the TableFilter hierarchy; the approximate kinds
		// (bloom, uninitialized dynamic) return the constant TRUE, which is safe here
		// because those are redundant with the join that produced them.
		auto expr = entry.second->ToExpression(*col_ref);
		if (!expr) {
			throw NotImplementedException("read_zim: could not rebuild a pushed-down filter as an expression");
		}
		conditions.push_back(std::move(expr));
	}
	if (conditions.size() == 1) {
		return std::move(conditions[0]);
	}
	auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (auto &condition : conditions) {
		conjunction->children.push_back(std::move(condition));
	}
	return std::move(conjunction);
}

// Keep only the rows of `scan_chunk` that satisfy the pushed-down filters, writing the
// survivors into `output`. Returns the surviving row count.
static idx_t SelectFilteredRows(ClientContext &context, const ReadZimGlobalState &g, ReadZimLocalState &l,
                                DataChunk &output) {
	if (!l.filter_executor) {
		l.filter_executor = make_uniq<ExpressionExecutor>(context, *g.filter_expression);
		l.filter_sel.Initialize(STANDARD_VECTOR_SIZE);
	}
	const idx_t count = l.filter_executor->SelectExpression(l.scan_chunk, l.filter_sel);
	if (count == l.scan_chunk.size()) {
		output.Reference(l.scan_chunk);
	} else {
		output.Slice(l.scan_chunk, l.filter_sel, count);
	}
	return count;
}

// `col = <varchar const>` -> sets `out`, returns true.
static bool FilterEqualityString(const TableFilter &filter, std::string &out) {
	if (filter.filter_type != TableFilterType::CONSTANT_COMPARISON) {
		return false;
	}
	auto &cf = filter.Cast<ConstantFilter>();
	if (cf.comparison_type != ExpressionType::COMPARE_EQUAL || cf.constant.IsNull() ||
	    cf.constant.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	out = cf.constant.GetValue<std::string>();
	return true;
}

// Append the varchar constants of an equality or IN filter to `out`. A pattern must
// keep MimetypeAccepted a superset of the filter: the empty string is skipped because
// MimetypePatternMatch never matches an empty mimetype, so pushing it would drop the
// very rows `mimetype = ''` selects.
static void CollectMimetypeConstants(const TableFilter &filter, std::vector<std::string> &out) {
	if (filter.filter_type == TableFilterType::CONSTANT_COMPARISON) {
		auto &cf = filter.Cast<ConstantFilter>();
		if (cf.comparison_type == ExpressionType::COMPARE_EQUAL && !cf.constant.IsNull() &&
		    cf.constant.type().id() == LogicalTypeId::VARCHAR) {
			auto value = cf.constant.GetValue<std::string>();
			if (!value.empty()) {
				out.push_back(std::move(value));
			}
		}
	} else if (filter.filter_type == TableFilterType::IN_FILTER) {
		std::vector<std::string> values;
		for (auto &v : filter.Cast<InFilter>().values) {
			if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR) {
				return; // cannot represent this IN list; leave the scan unfiltered
			}
			auto value = v.GetValue<std::string>();
			if (value.empty()) {
				return;
			}
			values.push_back(std::move(value));
		}
		for (auto &value : values) {
			out.push_back(std::move(value));
		}
	}
}

// Intersect the exact mimetype constants of a pushed-down filter with the Accept-style
// patterns already in the scan spec (the `mimetype :=` named parameter, possibly empty).
//
// Issue #34: these two must be AND-ed. `mimetype := 'text/css'` is an explicit argument
// that defines the rows the function produces; a WHERE clause can only narrow that set,
// never widen it. Appending the pushed constants to the same vector made MimetypeAccepted
// — an OR over its patterns — accept their union, so
// `read_zim(..., mimetype := 'text/css') WHERE mimetype = 'image/png'` returned the PNG.
//
// The intersection is exact rather than approximate: `constants` are literal mimetypes,
// so {m : m matches some pattern} INTERSECT {m : m IN constants} is precisely the subset
// of `constants` the patterns accept. An empty `patterns` accepts everything
// (MimetypeAccepted's own convention), which is the no-named-parameter case.
static std::vector<std::string> IntersectMimetypes(const std::vector<std::string> &patterns,
                                                   const std::vector<std::string> &constants) {
	std::vector<std::string> result;
	for (const auto &c : constants) {
		if (MimetypeAccepted(patterns, c)) {
			result.push_back(c);
		}
	}
	return result;
}

// Best-effort read reduction (layer 1 above). Correctness never depends on this: every
// pushed filter is re-applied exactly by g.filter_expression, so each rule here only has
// to keep the scan a superset of the true result.
static void ApplyPushedFilters(ReadZimGlobalState &g, TableFunctionInitInput &input) {
	if (!input.filters) {
		return;
	}
	// Only a plain path-order full scan may be redirected to a lookup; a named
	// path:=/title:=/prefix/listing keeps its mode (the WHERE filter is then applied
	// on the emitted rows like any other).
	const bool scan_is_open = !g.single_lookup && !g.scan_spec.path_prefix.has_value() &&
	                          !g.scan_spec.title_prefix.has_value() && g.scan_spec.order == ScanOrder::ByPath;
	// Mimetype constants pushed from WHERE, kept OUT of g.scan_spec until the whole
	// filter set has been walked: they narrow the named-parameter patterns, they do not
	// join them (issue #34).
	std::vector<std::string> pushed_mimetypes;
	bool have_pushed_mimetype = false;
	// filters are keyed by *projected* (output) column index; map back to storage. An
	// index we cannot map is skipped rather than raised: this layer only reads less, and
	// BuildFilterExpression — the layer results actually depend on — throws on exactly
	// the same condition a moment later, so nothing is silently dropped.
	for (auto &entry : input.filters->filters) {
		const idx_t proj_idx = entry.first;
		if (proj_idx >= g.column_ids.size()) {
			continue;
		}
		const column_t storage_col = g.column_ids[proj_idx];
		const TableFilter &filter = *entry.second;
		if (storage_col == COL_MIMETYPE) {
			std::vector<std::string> constants;
			CollectMimetypeConstants(filter, constants);
			// An unrepresentable filter yields no constants: keep whatever the other
			// entries gave us (a superset of the AND) instead of widening back out.
			if (!constants.empty()) {
				// The same column can carry more than one filter entry (it may be
				// projected twice); those are AND-ed, so intersect exactly.
				pushed_mimetypes =
				    have_pushed_mimetype ? IntersectMimetypes(pushed_mimetypes, constants) : std::move(constants);
				have_pushed_mimetype = true;
			}
		} else if (scan_is_open && !g.single_lookup && (storage_col == COL_PATH || storage_col == COL_TITLE)) {
			std::string key;
			if (FilterEqualityString(filter, key)) {
				g.single_lookup = true;
				g.lookup_by_title = (storage_col == COL_TITLE);
				g.lookup_key = std::move(key);
				g.mode = ScanMode::Lookup;
			}
		}
	}
	if (have_pushed_mimetype) {
		auto narrowed = IntersectMimetypes(g.scan_spec.mimetype_filter, pushed_mimetypes);
		if (!narrowed.empty()) {
			g.scan_spec.mimetype_filter = std::move(narrowed);
		}
		// An empty intersection means no mimetype can satisfy both. We cannot express
		// "accept nothing" here — an empty mimetype_filter means accept EVERYTHING — so
		// leave the named-parameter filter untouched; it is still a superset, and
		// g.filter_expression removes the remaining rows.
	}
}

unique_ptr<GlobalTableFunctionState> ReadZimInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadZimBindData>();
	auto state = make_uniq<ReadZimGlobalState>();
	state->mode = bind.mode;
	state->files = bind.file_paths;
	state->column_ids = input.column_ids;
	state->fs = &FileSystem::GetFileSystem(context); // used only when a path is remote
	state->pool = &GetArchivePool(context);          // per-DB pool from the context's ObjectCache

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
	state->scan_spec = bind.spec;
	state->scan_spec.want_content = state->want_content;
	// Decompression-bomb guard: cap the decompressed size of any entry this scan
	// materializes. Applies to the parallel/serial cursor (via scan_spec) and to the
	// exact-lookup path below.
	state->scan_spec.max_content_bytes = ResolveMaxContentSize(context);

	// Seed lookup params from the bind (path:=/title:=), then let a pushed-down
	// WHERE path/title = const turn a plain full scan into an exact lookup and
	// add any WHERE mimetype to the post-filter. This may flip state->mode to
	// Lookup, so resolve max_threads / open the first file *after* it runs.
	state->single_lookup = bind.single_lookup;
	state->lookup_by_title = bind.lookup_by_title;
	state->lookup_key = bind.lookup_key;
	ApplyPushedFilters(*state, input);
	// ...and rebuild every pushed filter as an expression the scan evaluates on each
	// chunk. DuckDB has already deleted these from the plan, so this is the only thing
	// standing between a pushed-down predicate and a silently wrong result.
	state->filter_expression = BuildFilterExpression(bind, *state, input);

	if (state->mode == ScanMode::ParallelScan) {
		// Use DuckDB's configured thread count; morsels (and their archives) are
		// dispensed lazily, so nothing is opened until a thread asks for work.
		state->max_threads = MaxValue<idx_t>(1, (idx_t)TaskScheduler::GetScheduler(context).NumberOfThreads());
	} else {
		state->max_threads = 1;
		OpenCurrentFile(*state, bind); // serial paths open the first archive eagerly
	}
	return std::move(state);
}

unique_ptr<LocalTableFunctionState> ReadZimInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                     GlobalTableFunctionState *gstate) {
	return make_uniq<ReadZimLocalState>();
}

void EmitRow(const ReadZimBindData &bind, const ReadZimGlobalState &gstate, const ZimEntry &row,
             const std::string &file_path, DataChunk &output, idx_t out_idx) {
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
			// content_loaded is the single source of truth: false for redirects,
			// for un-projected/opted-out scans, and for entries the include_content
			// mimetype gate skipped — all of which surface as NULL.
			if (row.is_redirect || !row.content_loaded) {
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
			// The archive this row came from (passed in: serial uses the current file,
			// parallel the morsel's file).
			vec.SetValue(out_idx, Value(file_path));
			break;
		default:
			break;
		}
	}
}

// Fills `output` with the next batch of raw scan rows (no pushed-down filters applied)
// and sets its cardinality. A cardinality of 0 means the scan is exhausted.
static void ProduceChunk(const ReadZimBindData &bind, ReadZimGlobalState &gstate, ReadZimLocalState &lstate,
                         DataChunk &output) {
	// --- parallel path-order scan: each thread drains cluster-order morsels. ---
	// libzim's Archive is threadsafe, so the morsels read the shared pooled handle
	// concurrently. Row order is not guaranteed (parallel scans never are).
	if (gstate.mode == ScanMode::ParallelScan) {
		idx_t count = 0;
		while (count < STANDARD_VECTOR_SIZE) {
			if (lstate.idx >= lstate.end) {
				if (!gstate.NextMorsel(lstate.file_idx, lstate.idx, lstate.end, lstate.archive)) {
					break; // every morsel dispensed and drained
				}
			}
			while (lstate.idx < lstate.end && count < STANDARD_VECTOR_SIZE) {
				auto row = lstate.archive->ScanIndex(lstate.idx, gstate.scan_spec);
				lstate.idx++;
				if (row.has_value()) {
					EmitRow(bind, gstate, *row, gstate.files[lstate.file_idx], output, count);
					count++;
				}
			}
		}
		output.SetCardinality(count);
		return;
	}

	// --- serial: exact lookup, or a single cursor (prefix range / title listing) ---
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.file_idx < gstate.files.size()) {
		if (gstate.single_lookup) {
			// Per-file exact lookup: emit at most one row for the current archive, then
			// move on. An exact path present in N archives therefore emits N rows.
			if (!gstate.lookup_emitted) {
				gstate.lookup_emitted = true;
				auto row = gstate.lookup_by_title ? gstate.archive->GetByTitle(gstate.lookup_key, gstate.want_content,
				                                                               gstate.scan_spec.max_content_bytes)
				                                  : gstate.archive->GetByPath(gstate.lookup_key, gstate.want_content,
				                                                              gstate.scan_spec.max_content_bytes);
				// Apply the include_content mimetype gate to the single fetched row,
				// matching the scan path: a non-matching entry keeps its metadata but
				// reports NULL content.
				if (row.has_value() && row->content_loaded && !gstate.scan_spec.content_mimetypes.empty() &&
				    !MimetypeAccepted(gstate.scan_spec.content_mimetypes, row->mimetype)) {
					row->content.clear();
					row->content_loaded = false;
				}
				if (row.has_value() && MimetypeAccepted(gstate.scan_spec.mimetype_filter, row->mimetype)) {
					EmitRow(bind, gstate, *row, gstate.files[gstate.file_idx], output, count);
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
				EmitRow(bind, gstate, row, gstate.files[gstate.file_idx], output, count);
				count++;
			} else {
				AdvanceFile(gstate, bind);
			}
		}
	}
	output.SetCardinality(count);
}

void ReadZimFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<ReadZimBindData>();
	auto &gstate = data.global_state->Cast<ReadZimGlobalState>();
	auto &lstate = data.local_state->Cast<ReadZimLocalState>();

	if (!gstate.filter_expression) {
		ProduceChunk(bind, gstate, lstate, output);
		return;
	}

	// Filters were pushed down: scan into the local chunk and select from it. An
	// entirely filtered-out batch must NOT be returned — a zero-row chunk is how a
	// table function reports end-of-scan — so keep scanning until something survives
	// or the scan really is exhausted.
	if (lstate.scan_chunk.ColumnCount() == 0) {
		lstate.scan_chunk.Initialize(Allocator::Get(context), output.GetTypes());
	}
	for (;;) {
		lstate.scan_chunk.Reset();
		ProduceChunk(bind, gstate, lstate, lstate.scan_chunk);
		if (lstate.scan_chunk.size() == 0) {
			output.SetCardinality(0);
			return;
		}
		if (SelectFilteredRows(context, gstate, lstate, output) > 0) {
			return;
		}
		// The whole batch was filtered out, so we have to keep scanning — and a zero-row
		// chunk is not an option: PhysicalTableScan reads chunk.size() == 0 as
		// SourceResultType::FINISHED (and the AsyncResultType::HAVE_MORE_OUTPUT escape
		// hatch is rejected with an empty chunk under the synchronous strategy), so
		// returning early would silently truncate the scan. Without a return there is no
		// cancellation point: in the parallel path ProduceChunk pulls fresh morsels
		// itself, so `WHERE path LIKE 'zzz%'` over a large archive would run the entire
		// scan inside one call and ignore an interrupt for its duration (issue #35).
		//
		// DuckDB's own table scan has exactly this loop and exactly this remedy — see
		// TableScanFunc in duckdb/src/function/table/table_scan.cpp, "Before looping
		// back, check if we are interrupted". Same idiom, same granularity (at most one
		// STANDARD_VECTOR_SIZE batch between checks).
		if (context.interrupted) {
			throw InterruptException();
		}
	}
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
		TableFunction f("read_zim", {arg_type}, ReadZimFunction, ReadZimBind, ReadZimInitGlobal, ReadZimInitLocal);
		// ANY so a BOOLEAN, a VARCHAR mimetype, or a LIST(VARCHAR) all bind.
		f.named_parameters["include_content"] = LogicalType::ANY;
		f.named_parameters["content_as_varchar"] = LogicalType::BOOLEAN;
		f.named_parameters["parallel"] = LogicalType::BOOLEAN;
		f.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
		f.named_parameters["filename"] = LogicalType::BOOLEAN;
		// ANY so a single mimetype or a LIST(VARCHAR) of patterns both bind.
		f.named_parameters["mimetype"] = LogicalType::ANY;
		f.named_parameters["path"] = LogicalType::VARCHAR;
		f.named_parameters["title"] = LogicalType::VARCHAR;
		f.named_parameters["path_prefix"] = LogicalType::VARCHAR;
		f.named_parameters["title_prefix"] = LogicalType::VARCHAR;
		f.named_parameters["listing"] = LogicalType::VARCHAR;
		f.projection_pushdown = true;
		// Accepting pushdown means OWNING every pushed filter: DuckDB deletes them from
		// the plan and never re-applies them (issue #29). ReadZimFunction evaluates the
		// whole TableFilterSet on each chunk; ApplyPushedFilters additionally turns
		// `path/title = const` into an exact libzim lookup and `mimetype = const` /
		// `mimetype IN (...)` into a scan-level skip, purely to read less.
		// filter_prune stays off, so a filtered column is always emitted by the scan and
		// the filter indices line up with the output chunk one-to-one.
		f.filter_pushdown = true;
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
