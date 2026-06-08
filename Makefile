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

# Build icu's host tools with the SAME triplet as the target. The reusable CI workflow
# leaves VCPKG_HOST_TRIPLET unset inside the build container, so vcpkg defaults the host
# to x64-linux (full) while the target is x64-linux-release. Building icu for two
# different triplets corrupts its shared buildtree and fails the target build -- this was
# the lone difference between a failing pipeline run and a known-good build with
# host == target. Only applies in CI (VCPKG_TARGET_TRIPLET is unset for local builds).
ifneq ($(VCPKG_TARGET_TRIPLET),)
VCPKG_HOST_TRIPLET ?= $(VCPKG_TARGET_TRIPLET)
export VCPKG_HOST_TRIPLET
endif

# Build vcpkg dependencies serially -- kept as belt-and-suspenders for icu determinism
# while stabilizing CI. DuckDB's own compile stays parallel (ninja).
export VCPKG_MAX_CONCURRENCY=1

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile