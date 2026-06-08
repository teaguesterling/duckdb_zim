cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME zim)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# DuckDB's top-level CMake pins CMAKE_CXX_STANDARD to 11 in the cache, so a CACHE
# set here is a silent no-op. A plain (non-CACHE) set shadows it for this dir, and
# target_compile_features below makes C++17 explicit per target (required for
# std::optional in zim_access.hpp).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Public extension header (zim_extension.hpp) lives in src/include/ — that is the
# path DuckDB's generated extension loader includes by convention. The internal
# headers (zim_access.hpp, zim_archive_pool.hpp) live flat in src/.
include_directories(src/include src)

set(EXTENSION_SOURCES
    src/zim_extension.cpp
    src/zim_access.cpp
    src/zim_archive_pool.cpp
    src/read_zim.cpp
    src/zim_metadata.cpp
    src/zim_scalars.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# libzim ships libzim.pc (vcpkg_fixup_pkgconfig in the port), so pkg-config is the
# reliable discovery path and pulls in its transitive deps (zstd/liblzma/icu/xapian).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIM REQUIRED IMPORTED_TARGET libzim)

# Link libzim into BOTH the static library and the loadable extension.
target_link_libraries(${EXTENSION_NAME} PkgConfig::LIBZIM)
target_link_libraries(${LOADABLE_EXTENSION_NAME} PkgConfig::LIBZIM)

# Force C++17 on our sources regardless of DuckDB's cached CMAKE_CXX_STANDARD=11.
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
