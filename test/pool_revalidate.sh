#!/usr/bin/env bash
# Compiles and runs test/pool_revalidate.cpp against the release build. Covers
# ArchivePool revalidation (#38): a .zim replaced on disk must stop being served
# from the pool's cached handle, while a zim:// handle already open over the old
# archive keeps working. See the header comment in the .cpp for why this cannot
# be a sqllogictest file.
#
# Usage, from the repo root:
#     bash test/pool_revalidate.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/release"
DUCKDB="$BUILD/duckdb"
EXT_SO="$(find "$BUILD" -name 'zim.duckdb_extension' | head -1)"

if [ ! -x "$DUCKDB" ] || [ -z "$EXT_SO" ]; then
	echo "SKIP: no release build found (run 'make release' first)" >&2
	exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

g++ -std=c++17 -O1 \
	-I"$ROOT/duckdb/src/include" \
	-o "$TMP/pool_revalidate" "$ROOT/test/pool_revalidate.cpp" \
	-L"$BUILD/src" -lduckdb -Wl,-rpath,"$BUILD/src"

"$TMP/pool_revalidate" "$EXT_SO" "$TMP"
