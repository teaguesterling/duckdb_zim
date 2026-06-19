#!/usr/bin/env bash
# Run the WASM dependency-symbol check against built zim .wasm artifacts.
#
# Guards the LINKED_LIBS class of bug (see check_wasm_imports.mjs): the loadable
# .duckdb_extension.wasm is produced by a separate `emcc -sSIDE_MODULE=2 ...
# ${LINKED_LIBS}` step that ignores target_link_libraries(). libzim (and its
# transitive zstd/lzma) must be named in this extension's LINKED_LIBS or their
# symbols are left unresolved and the module fails to load. Runtime-free: no
# duckdb-wasm engine / version match needed. Run after `make wasm_mvp`/`wasm_eh`
# or point it at downloaded CI artifacts.
#
# Usage: test/wasm/run_wasm_checks.sh [dir ...]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"

# Dependency symbols that must be resolved internally (not left unresolved):
#   libzim   -> zim:: namespace mangles to contain "3zim"
#   zstd/lzma-> ZSTD_* / lzma_* (libzim's cluster decompression)
#   icu/xapian -> guard against a future code path pulling them in unlinked
FORBID=( --forbid '3zim' --forbid 'libzim'
         --forbid 'ZSTD_' --forbid 'lzma_'
         --forbid 'icu_' --forbid '6icu_' --forbid '^ucnv' --forbid '^ucol'
         --forbid '[Xx]apian' )

dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build/wasm_mvp build/wasm_eh build/wasm_threads; do
    [ -d "$repo_root/$d" ] && dirs+=("$repo_root/$d")
  done
fi
if [ ${#dirs[@]} -eq 0 ]; then
  echo "no wasm build dirs found; build first (e.g. make wasm_mvp)" >&2
  exit 2
fi

found_any=0
rc=0
for d in "${dirs[@]}"; do
  while IFS= read -r -d '' wasm; do
    found_any=1
    echo "==> checking $wasm"
    node "$here/check_wasm_imports.mjs" "$wasm" "${FORBID[@]}" || rc=1
  done < <(find "$d" -name 'zim.duckdb_extension.wasm' -print0 2>/dev/null)
done
if [ "$found_any" -eq 0 ]; then
  echo "no zim.duckdb_extension.wasm found under: ${dirs[*]}" >&2
  exit 2
fi
exit $rc
