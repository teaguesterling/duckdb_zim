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
    src/zim_remote.cpp
    src/read_zim.cpp
    src/zim_metadata.cpp
    src/zim_scalars.cpp
    src/zim_filesystem.cpp
    src/zim_search.cpp)

# libzim ships libzim.pc (vcpkg_fixup_pkgconfig in the port), so pkg-config is the
# reliable discovery path and pulls in its transitive deps (zstd/liblzma/icu/xapian).
# Resolved BEFORE build_*_extension so the WASM block can set this extension's
# LINKED_LIBS (read when build_loadable_extension is invoked).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIM REQUIRED IMPORTED_TARGET libzim)

# WASM: the emcc -sSIDE_MODULE link only honors this extension's LINKED_LIBS, not
# target_link_libraries(). Feed libzim + its transitive static deps (icu/zstd/lzma) as
# literal archive paths, grouped (-Wl,--start-group) to resolve ICU's circular refs.
# pkg-config's static link set gives full paths; unused ICU data dead-strips so the
# .wasm stays ~0.5 MB. (PkgConfig::LIBZIM is interface-only, so a $<TARGET_FILE> genexpr
# in extension_config.cmake resolves empty — hence resolving the archives here.)
if(WASM_LOADABLE_EXTENSIONS)
    set(_zim_pc_libs ${LIBZIM_STATIC_LINK_LIBRARIES})
    if(NOT _zim_pc_libs)
        set(_zim_pc_libs ${LIBZIM_LINK_LIBRARIES})
    endif()
    set(_zim_wasm_archives "")
    foreach(_l IN LISTS _zim_pc_libs)
        # pkg-config may resolve vcpkg debug archives; prefer the release variant.
        string(REPLACE "/debug/lib/" "/lib/" _l "${_l}")
        if(_l MATCHES "\\.a$" AND EXISTS "${_l}")
            list(APPEND _zim_wasm_archives "${_l}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _zim_wasm_archives)
    if(_zim_wasm_archives)
        set(DUCKDB_EXTENSION_ZIM_LINKED_LIBS
            "-Wl,--start-group;${_zim_wasm_archives};-Wl,--end-group")
        message(STATUS "zim WASM link libs: ${DUCKDB_EXTENSION_ZIM_LINKED_LIBS}")
    else()
        message(WARNING "zim WASM: no static libzim archives resolved "
            "(LIBZIM_STATIC_LINK_LIBRARIES='${LIBZIM_STATIC_LINK_LIBRARIES}')")
    endif()
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

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
