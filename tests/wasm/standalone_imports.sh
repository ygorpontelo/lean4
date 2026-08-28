#!/usr/bin/env bash
# Test: standalone wasm imports are pure WASI Preview 1.
# Verifies the LEAN_WASM_STANDALONE runtime does not pull in Emscripten
# JS-runtime dependencies. All imports must be wasi_snapshot_preview1
# except the two env stubs (uv_strerror, emscripten_notify_memory_growth).
set -eu

STANDALONE_DIR="$(dirname "$(dirname "$BUILD_DIR")")/wasm_standalone/stage1"
SL="$STANDALONE_DIR/lib/lean"
SI="$STANDALONE_DIR/include"
if [ ! -f "$SL/libleanrt.a" ]; then
  echo "SKIP: standalone runtime not built ($SL/libleanrt.a missing)"
  exit 0
fi

for tool in emcc em++ wasm-tools; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIP: $tool not found"
    exit 0
  fi
done

# Build a minimal standalone wasm (same as standalone_hello.sh).
LEAN_FILE="$WASM_TMP/standalone_imports.lean"
cat > "$LEAN_FILE" <<'LEAN'
def main : IO Unit := IO.println "hello"
LEAN

run_only "${LEAN_CMD[@]}" -c "$WASM_TMP/standalone_imports.c" "$LEAN_FILE"
check_exit_is_success

cat > "$WASM_TMP/standalone_imports_stubs.c" <<'C'
#include <stddef.h>
const char* uv_strerror(int err) { (void)err; return "uv error"; }
void emscripten_notify_memory_growth(size_t sz) { (void)sz; }
C

CC_FLAGS=(
  -I "$SI"
  -DLEAN_EMSCRIPTEN -DLEAN_WASM_COMPONENT -DLEAN_WASM_STANDALONE
  -fwasm-exceptions -s WASM_LEGACY_EXCEPTIONS=0 -s STANDALONE_WASM -s ALLOW_MEMORY_GROWTH=1
)

run_only emcc "$WASM_TMP/standalone_imports.c" -c -o "$WASM_TMP/standalone_imports.o" "${CC_FLAGS[@]}"
check_exit_is_success
run_only emcc "$WASM_TMP/standalone_imports_stubs.c" -c -o "$WASM_TMP/standalone_imports_stubs.o" "${CC_FLAGS[@]}"
check_exit_is_success
run_only em++ "$WASM_TMP/standalone_imports.o" "$WASM_TMP/standalone_imports_stubs.o" \
  "$SL/libleanrt.a" "$SL/libleancpp.a" "$SL/libInit.a" \
  -o "$WASM_TMP/standalone_imports.wasm" "${CC_FLAGS[@]}" \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
check_exit_is_success

# Inspect imports with wasm-tools.
IMPORTS=$(wasm-tools print "$WASM_TMP/standalone_imports.wasm" 2>/dev/null | grep '^\s*(import' || true)

if [ -z "$IMPORTS" ]; then
  fail "no imports found in standalone wasm"
fi

TOTAL=$(echo "$IMPORTS" | wc -l)
WASI_COUNT=$(echo "$IMPORTS" | grep -c 'wasi_snapshot_preview1' || true)
ENV_COUNT=$(echo "$IMPORTS" | grep -c '"env"' || true)
OTHER_COUNT=$((TOTAL - WASI_COUNT - ENV_COUNT))

if [ "$OTHER_COUNT" -ne 0 ]; then
  echo "$IMPORTS" | grep -v 'wasi_snapshot_preview1' || true
  fail "found $OTHER_COUNT non-WASI imports (expected 0): $(echo "$IMPORTS" | grep -v 'wasi_snapshot_preview1')"
fi

echo "PASS: $TOTAL imports (all wasi_snapshot_preview1, no env dependencies)"


rm -f "$WASM_TMP/standalone_imports.c" "$WASM_TMP/standalone_imports.o" \
      "$WASM_TMP/standalone_imports_stubs.o" "$WASM_TMP/standalone_imports.wasm"