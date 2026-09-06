//===----------------------------------------------------------------------===//
// zim_metadata.cpp
//
// The metadata door (M namespace) + archive-level info, kept distinct from
// content (read_zim). Per docs/libzim-semantics.md these are two separate APIs.
//
//   read_zim_metadata(file)            -> table(key, value, raw[, file_path])
//   zim_metadata(file, key)            -> VARCHAR
//   zim_metadata_keys(file)            -> LIST(VARCHAR)
//   zim_counter(file)                  -> MAP(VARCHAR, BIGINT)   (parsed Counter)
//   zim_info(file)                     -> STRUCT(...)            (counts/flags/uuid)
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/file_system.hpp"
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

namespace duckdb {

using zim_ext::GetArchivePool;
using zim_ext::ZimArchive;
using zim_ext::ZimInfo;

namespace {

// Metadata values are mostly text but some are binary (e.g. Illustration PNG). Only
// surface valid UTF-8 in the VARCHAR `value`/scalar; binary stays in the `raw` BLOB.
static bool IsValidUtf8(const std::string &s) {
	return Utf8Proc::IsValid(s.data(), s.size());
}

//===--------------------------------------------------------------------===//
// read_zim_metadata table function
//===--------------------------------------------------------------------===//

struct MetadataBindData : public TableFunctionData {
	std::string file_path;
	bool include_filepath = false;
};

struct MetadataGlobalState : public GlobalTableFunctionState {
	std::shared_ptr<ZimArchive> archive;
	std::vector<std::string> keys;
	idx_t pos = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> MetadataBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                      vector<CompatName> &names) {
	auto bind = make_uniq<MetadataBindData>();
	bind->file_path = input.inputs[0].GetValue<string>();
	for (auto &kv : input.named_parameters) {
		if (kv.first == "include_filepath" || kv.first == "filename") {
			bind->include_filepath = BooleanValue::Get(kv.second);
		} else {
			throw BinderException("read_zim_metadata: unknown parameter '%s'", kv.first);
		}
	}
	names = {"key", "value", "raw"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BLOB};
	if (bind->include_filepath) {
		names.emplace_back("file_path");
		return_types.emplace_back(LogicalType::VARCHAR);
	}
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> MetadataInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MetadataBindData>();
	auto state = make_uniq<MetadataGlobalState>();
	state->archive = GetArchivePool(context).Get(bind.file_path, &FileSystem::GetFileSystem(context));
	state->keys = state->archive->MetadataKeys();
	return std::move(state);
}

void MetadataFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<MetadataBindData>();
	auto &gstate = data.global_state->Cast<MetadataGlobalState>();

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.pos < gstate.keys.size()) {
		const auto &key = gstate.keys[gstate.pos++];
		auto value = gstate.archive->Metadata(key);
		std::string v = value.value_or(std::string());

		output.data[0].SetValue(count, Value(key));
		// `value` carries text only (binary -> NULL); the bytes always live in `raw`.
		output.data[1].SetValue(count, IsValidUtf8(v) ? Value(v) : Value(LogicalType::VARCHAR));
		output.data[2].SetValue(count, Value::BLOB_RAW(v));
		if (bind.include_filepath) {
			output.data[3].SetValue(count, Value(bind.file_path));
		}
		count++;
	}
	CompatSetCardinality(output, count);
}

//===--------------------------------------------------------------------===//
// scalar helpers
//===--------------------------------------------------------------------===//

// Open through the per-DB pool reached from the execution context. `fs` lets the
// pool open remote (s3/http) archives via byte-range reads; ignored for local paths.
static std::shared_ptr<ZimArchive> OpenArg(ExpressionState &state, const Vector &file_vec, idx_t row) {
	auto fp = FlatVector::GetData<string_t>(file_vec)[row].GetString();
	auto &ctx = state.GetContext();
	return GetArchivePool(ctx).Get(fp, &FileSystem::GetFileSystem(ctx));
}

// zim_metadata(file, key) -> VARCHAR
void ZimMetadataScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &files = args.data[0];
	auto &keys = args.data[1];
	files.Flatten(args.size());
	keys.Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = OpenArg(state, files, i);
		auto key = FlatVector::GetData<string_t>(keys)[i].GetString();
		auto v = archive->Metadata(key);
		if (v.has_value() && IsValidUtf8(*v)) {
			result.SetValue(i, Value(*v));
		} else {
			// Absent, or binary (e.g. an illustration PNG): use read_zim_metadata's
			// `raw` BLOB column for bytes. NULL here rather than mangle.
			FlatVector::SetNull(result, i, true);
		}
	}
}

// zim_metadata_keys(file) -> LIST(VARCHAR)
void ZimMetadataKeysScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &files = args.data[0];
	files.Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = OpenArg(state, files, i);
		vector<Value> keys;
		for (auto &k : archive->MetadataKeys()) {
			keys.emplace_back(Value(k));
		}
		result.SetValue(i, Value::LIST(LogicalType::VARCHAR, std::move(keys)));
	}
}

// zim_counter(file) -> MAP(VARCHAR, BIGINT)
void ZimCounterScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &files = args.data[0];
	files.Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = OpenArg(state, files, i);
		vector<Value> keys, vals;
		for (auto &kv : archive->Counter()) {
			keys.emplace_back(Value(kv.first));
			vals.emplace_back(Value::BIGINT(kv.second));
		}
		result.SetValue(i, Value::MAP(LogicalType::VARCHAR, LogicalType::BIGINT, std::move(keys), std::move(vals)));
	}
}

// zim_info(file) -> STRUCT(...)
void ZimInfoScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &files = args.data[0];
	files.Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = OpenArg(state, files, i);
		ZimInfo info = archive->Info();
		child_list_t<Value> fields;
		fields.emplace_back("uuid", Value(info.uuid));
		fields.emplace_back("entry_count", Value::UBIGINT(info.entry_count));
		fields.emplace_back("all_entry_count", Value::UBIGINT(info.all_entry_count));
		fields.emplace_back("article_count", Value::UBIGINT(info.article_count));
		fields.emplace_back("media_count", Value::UBIGINT(info.media_count));
		fields.emplace_back("main_entry_path", Value(info.main_entry_path));
		fields.emplace_back("has_fulltext_index", Value::BOOLEAN(info.has_fulltext_index));
		fields.emplace_back("has_title_index", Value::BOOLEAN(info.has_title_index));
		fields.emplace_back("has_checksum", Value::BOOLEAN(info.has_checksum));
		fields.emplace_back("is_multipart", Value::BOOLEAN(info.is_multipart));
		fields.emplace_back("has_new_namespace_scheme", Value::BOOLEAN(info.has_new_namespace_scheme));
		fields.emplace_back("filesize", Value::UBIGINT(info.filesize));
		result.SetValue(i, Value::STRUCT(std::move(fields)));
	}
}

LogicalType ZimInfoType() {
	child_list_t<LogicalType> fields;
	fields.emplace_back("uuid", LogicalType::VARCHAR);
	fields.emplace_back("entry_count", LogicalType::UBIGINT);
	fields.emplace_back("all_entry_count", LogicalType::UBIGINT);
	fields.emplace_back("article_count", LogicalType::UBIGINT);
	fields.emplace_back("media_count", LogicalType::UBIGINT);
	fields.emplace_back("main_entry_path", LogicalType::VARCHAR);
	fields.emplace_back("has_fulltext_index", LogicalType::BOOLEAN);
	fields.emplace_back("has_title_index", LogicalType::BOOLEAN);
	fields.emplace_back("has_checksum", LogicalType::BOOLEAN);
	fields.emplace_back("is_multipart", LogicalType::BOOLEAN);
	fields.emplace_back("has_new_namespace_scheme", LogicalType::BOOLEAN);
	fields.emplace_back("filesize", LogicalType::UBIGINT);
	return LogicalType::STRUCT(std::move(fields));
}

} // namespace

void RegisterZimMetadata(ExtensionLoader &loader) {
	TableFunction meta("read_zim_metadata", {LogicalType::VARCHAR}, MetadataFunction, MetadataBind, MetadataInit);
	meta.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
	meta.named_parameters["filename"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(meta);

	// All four open an archive, so all four throw at execution time on a missing or
	// corrupt ZIM -- test/sql/zim_errors.test asserts exactly that for zim_info.
	// DuckDB v2.0 requires such a function to declare itself fallible; see the note on
	// RegisterFallible in zim_scalars.cpp for why the omission is invisible at compile
	// time, why it shows up only on an assertions-enabled build, and what the flag
	// also does on the pinned v1.5 (where it is advisory, and where setting it is both
	// truthful and strictly more conservative).
	auto register_fallible = [&loader](ScalarFunction fun) {
		fun.SetFallible();
		loader.RegisterFunction(std::move(fun));
	};
	register_fallible(ScalarFunction("zim_metadata", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                 ZimMetadataScalar));
	register_fallible(ScalarFunction("zim_metadata_keys", {LogicalType::VARCHAR},
	                                 LogicalType::LIST(LogicalType::VARCHAR), ZimMetadataKeysScalar));
	register_fallible(ScalarFunction("zim_counter", {LogicalType::VARCHAR},
	                                 LogicalType::MAP(LogicalType::VARCHAR, LogicalType::BIGINT), ZimCounterScalar));
	register_fallible(ScalarFunction("zim_info", {LogicalType::VARCHAR}, ZimInfoType(), ZimInfoScalar));
}

} // namespace duckdb
