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
#include "utf8proc_wrapper.hpp"

#include "zim_access.hpp"
#include "zim_archive_pool.hpp"

namespace duckdb {

using zim_ext::ArchivePool;
using zim_ext::ZimArchive;

namespace {

static bool IsValidUtf8(const std::string &s) {
	return Utf8Proc::IsValid(s.data(), s.size());
}

static std::shared_ptr<ZimArchive> Open(const Vector &files, idx_t row) {
	auto fp = FlatVector::GetData<string_t>(files)[row].GetString();
	return ArchivePool::Instance().Get(fp);
}

static bool LooksLikeText(const std::string &mimetype) {
	if (mimetype.rfind("text/", 0) == 0) {
		return true;
	}
	// common textual application/* types found in ZIMs
	return mimetype.find("javascript") != std::string::npos ||
	       mimetype.find("json") != std::string::npos ||
	       mimetype.find("xml") != std::string::npos; // includes image/svg+xml
}

void GetContent(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto content = archive->GetContent(path);
		if (content.has_value()) {
			result.SetValue(i, Value::BLOB_RAW(*content));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void GetText(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto mime = archive->GetMimetype(path);
		if (!mime.has_value() || !LooksLikeText(*mime)) {
			FlatVector::SetNull(result, i, true); // binary or absent -> NULL, never mangle
			continue;
		}
		auto content = archive->GetContent(path);
		// Mimetype said text, but guard the bytes too: a mislabeled entry with invalid
		// UTF-8 becomes NULL rather than throwing/mangling.
		if (content.has_value() && IsValidUtf8(*content)) {
			result.SetValue(i, Value(*content));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void HasEntry(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		result.SetValue(i, Value::BOOLEAN(archive->HasEntry(path)));
	}
}

void RedirectTarget(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto target = archive->GetRedirectTarget(path);
		if (target.has_value()) {
			result.SetValue(i, Value(*target));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void Mimetype(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	args.data[1].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = FlatVector::GetData<string_t>(args.data[1])[i].GetString();
		auto mime = archive->GetMimetype(path);
		if (mime.has_value()) {
			result.SetValue(i, Value(*mime));
		} else {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void MainEntry(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto archive = Open(args.data[0], i);
		auto path = archive->MainEntryPath();
		if (path.empty()) {
			FlatVector::SetNull(result, i, true);
		} else {
			result.SetValue(i, Value(path));
		}
	}
}

} // namespace

void RegisterZimScalars(ExtensionLoader &loader) {
	const auto V = LogicalType::VARCHAR;
	loader.RegisterFunction(
	    ScalarFunction("zim_get_content", {V, V}, LogicalType::BLOB, GetContent));
	loader.RegisterFunction(ScalarFunction("zim_get_text", {V, V}, V, GetText));
	loader.RegisterFunction(
	    ScalarFunction("zim_has_entry", {V, V}, LogicalType::BOOLEAN, HasEntry));
	loader.RegisterFunction(
	    ScalarFunction("zim_redirect_target", {V, V}, V, RedirectTarget));
	loader.RegisterFunction(ScalarFunction("zim_mimetype", {V, V}, V, Mimetype));
	loader.RegisterFunction(ScalarFunction("zim_main_entry", {V}, V, MainEntry));
}

} // namespace duckdb
