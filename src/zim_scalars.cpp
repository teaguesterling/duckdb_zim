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
#include "duckdb_compat.hpp"
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

// zim_check(file) -> BOOLEAN : libzim archive integrity check.
//
// Returns FALSE for an archive that cannot be OPENED, rather than raising.
//
// This is the one function in the family whose whole purpose is to answer "is
// this archive usable?", so it has to be answerable without aborting the query.
// Raising left callers with no way to ask at all: DuckDB's TRY() intercepts
// conversion and range errors, not the InvalidInputException thrown from the
// open path, so `TRY(zim_check(f))` propagates the error just the same.
//
// The concrete case is a shelf scanned from a directory where one file is
// truncated or is not a ZIM. Without this, a query over that shelf can only
// abort entirely, or use zim_search(ignore_errors := true) -- which skips the
// bad archive silently, so the caller gets fewer results and no way to learn
// that a corpus was dropped.
//
// Only the OPEN is guarded. An archive that opens but fails its integrity check
// already returns false, and anything CheckIntegrity() itself throws still
// propagates -- that is a real fault, not an unreadable file.
//
// CAVEAT, and it limits what this function can be believed to mean: it verifies
// internal CONSISTENCY, not COMPLETENESS. An archive whose writer died part-way
// through is self-consistent -- it opens, its checksum validates, and this
// returns true -- while containing only the entries written before the failure.
// Confirmed empirically: a libzim Creator aborted mid-write by a duplicate-path
// error leaves a file that both libzim and read_zim open happily, with a valid
// checksum, holding a strict prefix of the intended entries. The ZIM format does
// not record an expected entry count, so no open-and-verify check can detect it.
// Callers needing completeness must compare zim_info().entry_count against an
// external expectation.
void Check(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		std::shared_ptr<ZimArchive> archive;
		try {
			archive = Open(state, args.data[0], i);
		} catch (const std::exception &) {
			// Unopenable: not a ZIM, truncated, missing, or unreadable.
			result.SetValue(i, Value::BOOLEAN(false));
			continue;
		}
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

// EVERY scalar function in this extension can throw at execution time: each one
// opens an archive through the pool, and a missing, unreadable or corrupt ZIM
// raises from there rather than returning NULL (test/sql/zim_errors.test asserts
// exactly that for `SELECT zim_info('/no/such/archive.zim')`). DuckDB v2.0 requires
// a scalar function that can throw to declare it; throwing from one that has not
// is an InternalException naming SetFallible. That check is an ASSERTION, so it
// fires only on an assertions-enabled build -- invisible at compile time, and able
// to be red on one CI arch while green on another for the very same commit.
//
// Routed through one helper so a scalar cannot be added without walking past the
// reason. CompatSetFallible is a no-op on the pinned v1.5, which has no such
// contract, so registration there is unchanged.
static void RegisterFallible(ExtensionLoader &loader, ScalarFunction fun) {
	CompatSetFallible(fun);
	loader.RegisterFunction(std::move(fun));
}

void RegisterZimScalars(ExtensionLoader &loader) {
	const auto V = LogicalType::VARCHAR;
	RegisterFallible(loader, ScalarFunction("zim_get_content", {V, V}, LogicalType::BLOB, GetContent));
	RegisterFallible(loader, ScalarFunction("zim_get_text", {V, V}, V, GetText));
	RegisterFallible(loader, ScalarFunction("zim_has_entry", {V, V}, LogicalType::BOOLEAN, HasEntry));
	RegisterFallible(loader, ScalarFunction("zim_redirect_target", {V, V}, V, RedirectTarget));
	RegisterFallible(loader, ScalarFunction("zim_mimetype", {V, V}, V, Mimetype));
	RegisterFallible(loader, ScalarFunction("zim_main_entry", {V}, V, MainEntry));
	RegisterFallible(loader, ScalarFunction("zim_random", {V}, V, Random));
	RegisterFallible(loader, ScalarFunction("zim_check", {V}, LogicalType::BOOLEAN, Check));

	// zim_illustration(file) defaults to 48px; zim_illustration(file, size) is explicit.
	// Marked fallible BEFORE AddFunction: a v2.0 FunctionSet yields shared_ptr<const
	// T>, so a member cannot be configured once it is in the set.
	ScalarFunctionSet illustration("zim_illustration");
	ScalarFunction illustration_default({V}, LogicalType::BLOB, Illustration);
	CompatSetFallible(illustration_default);
	illustration.AddFunction(std::move(illustration_default));
	ScalarFunction illustration_sized({V, LogicalType::INTEGER}, LogicalType::BLOB, Illustration);
	CompatSetFallible(illustration_sized);
	illustration.AddFunction(std::move(illustration_sized));
	loader.RegisterFunction(illustration);
}

} // namespace duckdb
