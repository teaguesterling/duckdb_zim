PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=zim
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Build the WHOLE tree (DuckDB + this extension) at C++17. The extension needs it
# (std::optional in the access layer), and it must match DuckDB's standard: DuckDB
# defaults CMAKE_CXX_STANDARD to 11, and mixing 11/17 makes its `static constexpr`
# LogicalType constants (VARCHAR/BOOLEAN/UBIGINT) inline on one side only -> "multiple
# definition" at link time. EXT_FLAGS feeds BUILD_FLAGS, so this reaches DuckDB's own
# configure here and in CI (the reusable workflow runs this Makefile's `make` target).
EXT_FLAGS=-DCMAKE_CXX_STANDARD=17

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile