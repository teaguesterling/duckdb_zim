#!/usr/bin/env bash
# Integration test for remote full-text search over http.
#
# zim_search over an http(s) URL has no local file for Xapian to mmap. libzim handles it
# two ways (see docs/dev/remote-search-design.md + phase-b-progress.md):
#   - index <= zim_remote_search_max_local_index: copy the index blob to a temp file;
#   - otherwise: range-read the index in place (Xapian fetches only the blocks it needs).
# Both need a range-capable server, so this uses test/remote/range_http_server.py (plain
# `python3 -m http.server` has no Range support). sqllogictest can't host a server, so we
# serve the committed test/oracle/test.zim and assert remote results match the local
# search in test/sql/zim_search.test -- including the over-cap range-reader path.
#
# Env overrides: DUCKDB_BIN, ZIM_EXTENSION (paths to the built shell + loadable ext).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$repo/build/release/duckdb}"
ZIM_EXTENSION="${ZIM_EXTENSION:-$repo/build/release/extension/zim/zim.duckdb_extension}"
fixture_dir="$repo/test/oracle"
fixture="test.zim"

[ -x "$DUCKDB_BIN" ] || { echo "SKIP: duckdb shell not found at $DUCKDB_BIN (build first)"; exit 0; }
[ -f "$ZIM_EXTENSION" ] || { echo "SKIP: zim extension not found at $ZIM_EXTENSION (build first)"; exit 0; }
[ -f "$fixture_dir/$fixture" ] || { echo "FAIL: fixture missing: $fixture_dir/$fixture"; exit 1; }

# Pick a free port.
port="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

python3 "$here/range_http_server.py" "$port" "$fixture_dir" 127.0.0.1 >/dev/null 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT

# Wait for the server to accept connections (up to ~5s).
for _ in $(seq 1 50); do
  if curl -sf -o /dev/null "http://127.0.0.1:$port/$fixture"; then break; fi
  sleep 0.1
done

url="http://127.0.0.1:$port/$fixture"
sql="LOAD '$ZIM_EXTENSION'; INSTALL httpfs; LOAD httpfs;
     SELECT path FROM zim_search('$url', 'plants') ORDER BY path;"
out="$("$DUCKDB_BIN" -unsigned -noheader -list -c "$sql" 2>/dev/null || true)"

echo "--- remote zim_search('$url', 'plants') ---"
echo "$out"

fail=0
for expect in "A/Chlorophyll" "A/Photosynthesis"; do
  if ! grep -qx "$expect" <<<"$out"; then echo "FAIL: missing expected hit: $expect"; fail=1; fi
done

# A narrower query should still resolve over http (exercises the index, not a full scan).
sql2="LOAD '$ZIM_EXTENSION'; INSTALL httpfs; LOAD httpfs;
      SELECT count(*) FROM zim_search('$url', 'photosynthesis');"
n="$("$DUCKDB_BIN" -unsigned -noheader -list -c "$sql2" 2>/dev/null | tail -1 || echo 0)"
if [ "${n:-0}" -lt 1 ]; then echo "FAIL: narrow remote query returned no hits ($n)"; fail=1; fi

# has_fulltext_index reports index *existence* over http (not copyability): a remote
# archive whose index exceeds the cap must still report true (regression guard for the
# bug where it returned false for big-index remote archives).
fts="$("$DUCKDB_BIN" -unsigned -noheader -list \
  -c "LOAD '$ZIM_EXTENSION'; INSTALL httpfs; LOAD httpfs; SELECT zim_info('$url').has_fulltext_index;" 2>/dev/null | tail -1)"
if [ "$fts" != "true" ]; then echo "FAIL: remote has_fulltext_index='$fts' (expected true)"; fail=1; fi

# Phase B: with the local-copy cap forced to 1 byte, the index exceeds it, so search
# must fall back to range-reading the index in place -- and still return the same hits
# (not an error, and not empty). This exercises the random-access glass reader path.
overcap="$("$DUCKDB_BIN" -unsigned -noheader -list \
  -c "LOAD '$ZIM_EXTENSION'; INSTALL httpfs; LOAD httpfs; SET zim_remote_search_max_local_index=1; SELECT path FROM zim_search('$url','plants') ORDER BY path;" 2>&1 || true)"
echo "--- over-cap (range-reader) zim_search('plants') ---"
echo "$overcap"
for expect in "A/Chlorophyll" "A/Photosynthesis"; do
  if ! grep -qx "$expect" <<<"$overcap"; then
    echo "FAIL: over-cap range-reader search missing '$expect'; got: $overcap"; fail=1
  fi
done

# zim_suggest (title index) over http must also work -- default cap (copy path) and
# forced over-cap (range-read the title index in place). The title index has its own
# loader in libzim; regression guard against it silently returning nothing remotely.
for cap_sql in "" "SET zim_remote_search_max_local_index=1;"; do
  label="default cap"; [ -n "$cap_sql" ] && label="over-cap (range-reader)"
  sug="$("$DUCKDB_BIN" -unsigned -noheader -list \
    -c "LOAD '$ZIM_EXTENSION'; INSTALL httpfs; LOAD httpfs; $cap_sql SELECT title FROM zim_suggest('$url','photo');" 2>&1 || true)"
  echo "--- $label zim_suggest('photo') ---"
  echo "$sug"
  if ! grep -qx "Photosynthesis" <<<"$sug"; then
    echo "FAIL: remote zim_suggest ($label) missing 'Photosynthesis'; got: $sug"; fail=1
  fi
done

if [ "$fail" -eq 0 ]; then echo "PASS: remote zim_search + zim_suggest match local search"; fi
exit "$fail"
