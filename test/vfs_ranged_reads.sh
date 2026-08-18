#!/usr/bin/env bash
# Compiles and runs test/vfs_ranged_reads.cpp against the release build, over a
# ZIM archive built on the fly with COPY ... (FORMAT zim). Covers the zim://
# handle's ranged-read semantics, which have no SQL consumer (see the header
# comment in the .cpp for why).
#
# Usage: bash test/no_stdout_pollution.sh-style, from the repo root:
#     bash test/vfs_ranged_reads.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/release"
EXT="$BUILD/repository"
DUCKDB="$BUILD/duckdb"
EXT_SO="$(find "$BUILD" -name 'zim.duckdb_extension' | head -1)"

if [ ! -x "$DUCKDB" ] || [ -z "$EXT_SO" ]; then
	echo "SKIP: no release build found (run 'make release' first)" >&2
	exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Fixture: a 1 MiB non-repeating binary entry (so an off-by-one in a ranged read
# cannot coincidentally match), an empty entry, and a small text entry.
"$DUCKDB" -c "
COPY (SELECT 'big.bin' AS path,
             from_hex(string_agg(md5(i::VARCHAR), '' ORDER BY i)) AS content
      FROM range(65536) t(i)
      UNION ALL SELECT 'empty.bin', ''::BLOB
      UNION ALL SELECT 'small.txt', 'hello ranged reads'::BLOB)
TO '$TMP/ranged.zim' (FORMAT zim);
" >/dev/null

g++ -std=c++17 -O1 \
	-I"$ROOT/duckdb/src/include" \
	-o "$TMP/vfs_ranged_reads" "$ROOT/test/vfs_ranged_reads.cpp" \
	-L"$BUILD/src" -lduckdb -Wl,-rpath,"$BUILD/src"

"$TMP/vfs_ranged_reads" "$EXT_SO" "$TMP/ranged.zim" big.bin empty.bin small.txt
