//===----------------------------------------------------------------------===//
// zim_extension.cpp — registration glue only.
//
// Build wiring (CMakeLists, vcpkg libzim dependency, extension_config.cmake,
// submodules) lives outside this file. This translation unit just assembles the
// core functions into the extension's Load() against DuckDB's ExtensionLoader API.
//===----------------------------------------------------------------------===//
#define DUCKDB_EXTENSION_MAIN

#include "zim_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Defined in the respective translation units.
void RegisterReadZim(ExtensionLoader &loader);
void RegisterZimMetadata(ExtensionLoader &loader);
void RegisterZimScalars(ExtensionLoader &loader);
void RegisterZimFilesystem(ExtensionLoader &loader); // phase 2 (zim://)
void RegisterZimSearch(ExtensionLoader &loader);     // phase 3 (xapian FTS)

static void LoadInternal(ExtensionLoader &loader) {
	RegisterReadZim(loader);
	RegisterZimMetadata(loader);
	RegisterZimScalars(loader);
	RegisterZimFilesystem(loader);
	RegisterZimSearch(loader);
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
