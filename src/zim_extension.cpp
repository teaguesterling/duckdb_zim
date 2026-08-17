//===----------------------------------------------------------------------===//
// zim_extension.cpp — registration glue only.
//
// Build wiring (CMakeLists, vcpkg libzim dependency, extension_config.cmake,
// submodules) lives outside this file. This translation unit just assembles the
// core functions into the extension's Load() against DuckDB's ExtensionLoader API.
//===----------------------------------------------------------------------===//
#define DUCKDB_EXTENSION_MAIN

#include "zim_extension.hpp"
#include "zim_access.hpp" // zim_ext::DEFAULT_MAX_CONTENT_SIZE
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Defined in the respective translation units.
void RegisterReadZim(ExtensionLoader &loader);
void RegisterZimMetadata(ExtensionLoader &loader);
void RegisterZimScalars(ExtensionLoader &loader);
void RegisterZimFilesystem(ExtensionLoader &loader); // phase 2 (zim://)
void RegisterZimSearch(ExtensionLoader &loader);     // phase 3 (xapian FTS)
void RegisterCopyToZim(ExtensionLoader &loader);     // phase 4 (COPY TO)

static void LoadInternal(ExtensionLoader &loader) {
	// The archive pool lives in each DatabaseInstance's ObjectCache (see
	// zim_archive_pool.hpp); consumers reach it via their ClientContext or, for the
	// zim:// VFS, the DatabaseInstance captured at registration.
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .AddExtensionOption("zim_remote_search_max_local_index",
	                        "Maximum size (bytes) of a remote ZIM's full-text index to copy locally so "
	                        "search works; 0 disables remote search. Larger values cover bigger archives "
	                        "but lengthen the first remote search (the whole index is fetched once).",
	                        LogicalType::UBIGINT, Value::UBIGINT(8ull * 1024 * 1024));

	// Ceiling (bytes) on the decompressed size of any single ZIM entry the extension
	// will materialize into memory. ZIM clusters are compressed, so a small crafted
	// archive can declare a huge uncompressed item and force an unbounded allocation
	// (decompression bomb). Reads of an entry larger than this fail cleanly before
	// allocating. 0 disables the cap. Default 2 GiB — generous enough for normal
	// articles and typical embedded media; lower it when reading untrusted archives.
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .AddExtensionOption("zim_max_content_size",
	                        "Maximum decompressed size (bytes) of a single ZIM entry to materialize; reads "
	                        "of a larger entry fail cleanly instead of allocating (decompression-bomb guard). "
	                        "0 disables the cap. Lower it when reading untrusted archives.",
	                        LogicalType::UBIGINT, Value::UBIGINT(zim_ext::DEFAULT_MAX_CONTENT_SIZE));

	RegisterReadZim(loader);
	RegisterZimMetadata(loader);
	RegisterZimScalars(loader);
	RegisterZimFilesystem(loader);
	RegisterZimSearch(loader);
	RegisterCopyToZim(loader);
}

void ZimExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string ZimExtension::Name() {
	return "zim";
}

std::string ZimExtension::Version() const {
#ifdef EXT_VERSION_ZIM
	return EXT_VERSION_ZIM;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(zim, loader) {
	duckdb::LoadInternal(loader);
}
}
