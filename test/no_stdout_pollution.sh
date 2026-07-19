#!/usr/bin/env bash
# Regression test for issue #21: searching a ZIM whose language Xapian cannot
# stem (e.g. Chinese) must not print anything to stdout but the query result.
#
# Upstream libzim does `std::cout << "No stemming for language 'X'"` when it
# can't build a stemmer for the archive's language, polluting the caller's
# stdout on every search/suggest open. Our libzim overlay patch
# (vcpkg_ports/libzim/no-stemming-stdout.patch) drops that print. This asserts
# it stays gone -- something sqllogictest can't check, because it compares query
# results via the API and never inspects process stdout.
#
# Fixture: test/oracle/test_zh.zim (built by test/oracle/make_zh_fixture.py),
# a tiny Chinese-language archive with a fulltext index.
#
# Env overrides (match test/remote/remote_search.sh): DUCKDB_BIN, ZIM_EXTENSION.
# The extension is LOAD-ed explicitly so this works with a stock DuckDB CLI in CI
# as well as the locally-built shell.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$repo/build/release/duckdb}"
ZIM_EXTENSION="${ZIM_EXTENSION:-$repo/build/release/extension/zim/zim.duckdb_extension}"
fixture="$repo/test/oracle/test_zh.zim"

[ -x "$DUCKDB_BIN" ] || { echo "SKIP: duckdb shell not found at $DUCKDB_BIN (build first)"; exit 0; }
[ -f "$ZIM_EXTENSION" ] || { echo "SKIP: zim extension not found at $ZIM_EXTENSION (build first)"; exit 0; }
[ -f "$fixture" ] || { echo "FAIL: fixture missing: $fixture (run test/oracle/make_zh_fixture.py)"; exit 1; }

# Capture stdout ONLY (drop stderr): the offending print goes to std::cout.
stdout="$("$DUCKDB_BIN" -unsigned -noheader -list \
  -c "LOAD '$ZIM_EXTENSION'; SELECT title FROM zim_search('$fixture', '矩阵') ORDER BY title;" 2>/dev/null)"

echo "--- stdout of zim_search(test_zh.zim, '矩阵') ---"
echo "$stdout"

fail=0

# 1) The search path actually ran (so the assertion is meaningful): expect hits.
if ! grep -q "矩阵" <<<"$stdout"; then
  echo "FAIL: expected Chinese search to return hits containing '矩阵'"; fail=1
fi

# 2) The regression guard: no libzim stdout pollution leaked into the output.
if grep -q "No stemming" <<<"$stdout"; then
  echo "FAIL: libzim leaked 'No stemming ...' to stdout (issue #21 regressed)"; fail=1
fi

if [ "$fail" -eq 0 ]; then echo "PASS: Chinese search works and stdout is clean (#21)"; fi
exit "$fail"
