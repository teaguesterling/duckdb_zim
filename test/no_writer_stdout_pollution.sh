#!/usr/bin/env bash
# Regression test: writing a ZIM must not print anything to stdout but the query
# result.
#
# libzim's writer defines INFO(e) as an UNGATED `std::cout << e`, unlike TINFO /
# TPROGRESS which check m_verbose. configVerbose(false) therefore cannot silence
# it. Our overlay patch (vcpkg_ports/libzim/no-writer-stdout.patch) drops the
# prints. This asserts they stay gone -- something sqllogictest can't check,
# because it compares query results via the API and never inspects stdout.
#
# Fixture note: this COPY uses a VALID redirect (A/Alias -> A/One, which the
# same query also produces), not a dangling one. A dangling redirect would
# exercise two more of INFO()'s six call sites (the invalid-redirection removal
# messages) -- but as of this task, ZimCopyFinalize (src/copy_to_zim.cpp)
# validates every redirect target against the paths actually written and
# throws *before* libzim ever sees the write (design §7.4: libzim otherwise
# drops dangling redirects silently at finishZimCreation(), announcing the
# drop only through the very prints this patch removes). So those two sites
# are now unreachable from SQL by construction, which is a better state than
# being able to trigger them: confirm the patch covers them by inspecting
# vcpkg_ports/libzim/no-writer-stdout.patch itself (it patches the INFO macro
# definition that all six call sites expand through), not by trying to fire
# them from here.
#
# Env overrides match test/no_stdout_pollution.sh: DUCKDB_BIN, ZIM_EXTENSION.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$repo/build/release/duckdb}"
ZIM_EXTENSION="${ZIM_EXTENSION:-$repo/build/release/extension/zim/zim.duckdb_extension}"

[ -x "$DUCKDB_BIN" ] || { echo "SKIP: duckdb shell not found at $DUCKDB_BIN (build first)"; exit 0; }
[ -f "$ZIM_EXTENSION" ] || { echo "SKIP: zim extension not found at $ZIM_EXTENSION (build first)"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
out="$tmp/written.zim"

# A write that exercises the indexing and redirect-resolution paths, since
# those are where four of INFO()'s six call sites live on any normal write.
stdout="$("$DUCKDB_BIN" -unsigned -noheader -list -c "
LOAD '$ZIM_EXTENSION';
COPY (SELECT * FROM (VALUES
        ('A/One',   'One',   'text/html', '<html><body>alpha</body></html>', false, NULL),
        ('A/Two',   'Two',   'text/html', '<html><body>beta</body></html>',  false, NULL),
        ('A/Alias', 'Alias', NULL,        NULL, true, 'A/One'))
      t(path, title, mimetype, content, is_redirect, redirect_path))
TO '$out' (FORMAT zim, LANGUAGE 'eng', INDEX true);
SELECT 'WROTE_OK';" 2>/dev/null)"

echo "--- stdout of COPY ... TO ... (FORMAT zim) ---"
echo "$stdout"

fail=0

# 1) The write actually ran, so the assertion is meaningful.
if ! grep -q "WROTE_OK" <<<"$stdout"; then
  echo "FAIL: the COPY did not complete; assertion would be vacuous"; fail=1
fi

# 2) The regression guard: none of libzim's INFO() strings leaked to stdout.
for s in "Set entry indices" "Index titles" "Detect dangling redirects" \
         "Detect loops and/or blind chains of redirects" \
         "Removing invalid redirection" "Redirection "; do
  if grep -qF "$s" <<<"$stdout"; then
    echo "FAIL: libzim leaked '$s' to stdout"; fail=1
  fi
done

if [ "$fail" -eq 0 ]; then echo "PASS: COPY TO zim works and stdout is clean"; fi
exit "$fail"
