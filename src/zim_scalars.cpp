//===----------------------------------------------------------------------===//
// zim_scalars.cpp — single-entry lookup scalars (mirror the md_* family).
//
//   zim_get_content(file, path)    -> BLOB     (redirect-following)
//   zim_get_text(file, path)       -> VARCHAR  (text mimetypes; NULL otherwise)
//   zim_has_entry(file, path)      -> BOOLEAN
//   zim_redirect_target(file, path)-> VARCHAR  (NULL if not a redirect)
//   zim_main_entry(file)           -> VARCHAR  (redirect-resolved landing path)
//   zim_mimetype(file, path)       -> VARCHAR
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/file_system.hpp"
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

namespace duckdb {

using zim_ext::GetArchivePool;
using zim_ext::ZimArchive;

namespace {

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

// Open through the per-DB pool reached from the execution context. `fs` lets the
// pool open remote (s3/http) archives via byte-range reads; ignored for local paths.
static std::shared_ptr<ZimArchive> Open(ExpressionState &state, const Vector &files, idx_t row) {
	auto fp = FlatVector::GetData<string_t>(files)[row].GetString();
	auto &ctx = state.GetContext();
	return GetArchivePool(ctx).Get(fp, &FileSystem::GetFileSystem(ctx));
}

static bool LooksLikeText(const std::string &mimetype) {
	if (mimetype.rfind("text/", 0) == 0) {
		return true;
	}
	// common textual application/* types found in ZIMs
	return mimetype.find("javascript") != std::string::npos || mimetype.find("json") != std::string::npos ||
	       mimetype.find("xml") != std::string::npos; // includes image/svg+xml
}

void GetContent(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	const uint64_t max_content = ResolveMaxContentSize(state.GetContext());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto content = archive->GetContent(path, max_content);
		if (content.has_value()) {
			result.SetValue(i, Value::BLOB_RAW(*content));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void GetText(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	const uint64_t max_content = ResolveMaxContentSize(state.GetContext());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto mime = archive->GetMimetype(path);
		if (!mime.has_value() || !LooksLikeText(*mime)) {
			FlatVector::SetNull(result, i, true); // binary or absent -> NULL, never mangle
			continue;
		}
		auto content = archive->GetContent(path, max_content);
		// Mimetype said text, but guard the bytes too: a mislabeled entry with invalid
		// UTF-8 becomes NULL rather than throwing/mangling.
		if (content.has_value() && IsValidUtf8(*content)) {
			result.SetValue(i, Value(*content));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void HasEntry(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		result.SetValue(i, Value::BOOLEAN(archive->HasEntry(path)));
	}
}

void RedirectTarget(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto target = archive->GetRedirectTarget(path);
		if (target.has_value()) {
			result.SetValue(i, Value(*target));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void Mimetype(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto mime = archive->GetMimetype(path);
		if (mime.has_value()) {
			result.SetValue(i, Value(*mime));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void MainEntry(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = archive->MainEntryPath();
		if (path.empty()) {
			FlatVector::SetNull(result, i, true);
		} else {
			result.SetValue(i, Value(path));
		}
	}
}

// zim_random(file) -> VARCHAR : a random content entry's path
void Random(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		auto path = archive->RandomPath();
		if (path.empty()) {
			FlatVector::SetNull(result, i, true);
		} else {
			result.SetValue(i, Value(path));
		}
	}
}

// zim_check(file) -> BOOLEAN : libzim archive integrity check
void Check(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		result.SetValue(i, Value::BOOLEAN(archive->CheckIntegrity()));
	}
}

// zim_illustration(file[, size]) -> BLOB : the archive's cover/favicon PNG (default 48px)
void Illustration(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	const bool has_size = args.ColumnCount() > 1;
	args.data[0].Flatten(args.size());
	if (has_size) {
		args.data[1].Flatten(args.size());
	}
	const uint64_t max_content = ResolveMaxContentSize(state.GetContext());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(state, args.data[0], i);
		unsigned int size = 48;
		if (has_size && FlatVector::Validity(args.data[1]).RowIsValid(i)) {
			size = static_cast<unsigned int>(FlatVector::GetData<int32_t>(args.data[1])[i]);
		}
		auto blob = archive->Illustration(size, max_content);
		if (blob.has_value()) {
			result.SetValue(i, Value::BLOB_RAW(*blob));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

} // namespace

void RegisterZimScalars(ExtensionLoader &loader) {
	const auto V = LogicalType::VARCHAR;
	loader.RegisterFunction(ScalarFunction("zim_get_content", {V, V}, LogicalType::BLOB, GetContent));
	loader.RegisterFunction(ScalarFunction("zim_get_text", {V, V}, V, GetText));
	loader.RegisterFunction(ScalarFunction("zim_has_entry", {V, V}, LogicalType::BOOLEAN, HasEntry));
	loader.RegisterFunction(ScalarFunction("zim_redirect_target", {V, V}, V, RedirectTarget));
	loader.RegisterFunction(ScalarFunction("zim_mimetype", {V, V}, V, Mimetype));
	loader.RegisterFunction(ScalarFunction("zim_main_entry", {V}, V, MainEntry));
	loader.RegisterFunction(ScalarFunction("zim_random", {V}, V, Random));
	loader.RegisterFunction(ScalarFunction("zim_check", {V}, LogicalType::BOOLEAN, Check));

	// zim_illustration(file) defaults to 48px; zim_illustration(file, size) is explicit.
	ScalarFunctionSet illustration("zim_illustration");
	illustration.AddFunction(ScalarFunction({V}, LogicalType::BLOB, Illustration));
	illustration.AddFunction(ScalarFunction({V, LogicalType::INTEGER}, LogicalType::BLOB, Illustration));
	loader.RegisterFunction(illustration);
}

} // namespace duckdb
