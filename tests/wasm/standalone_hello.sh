#!/usr/bin/env bash
# Test: standalone wasm pipeline — Lean → C → standalone wasm → wasmtime.
# Verifies the LEAN_WASM_STANDALONE runtime produces a working wasm module
# that runs in wasmtime (not just under node.js).
set -eu

# Skip if standalone runtime libs are not built.
STANDALONE_DIR="$(dirname "$(dirname "$BUILD_DIR")")/wasm_standalone/stage1"
SL="$STANDALONE_DIR/lib/lean"
SI="$STANDALONE_DIR/include"
if [ ! -f "$SL/libleanrt.a" ]; then
  echo "SKIP: standalone runtime not built ($SL/libleanrt.a missing)"
  exit 0
fi

# Skip if required tools are missing.
for tool in emcc em++ wasm-tools wasmtime; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIP: $tool not found"
    exit 0
  fi
done
WASMTIME="${WASMTIME:-$(command -v wasmtime)}"

# 1. Compile a trivial Lean program to C.
LEAN_FILE="$WASM_TMP/standalone_hello.lean"
cat > "$LEAN_FILE" <<'LEAN'
def main : IO Unit := IO.println "hello from lean"
LEAN

run_only "${LEAN_CMD[@]}" -c "$WASM_TMP/standalone_hello.c" "$LEAN_FILE"
check_exit_is_success
if [ ! -f "$WASM_TMP/standalone_hello.c" ]; then
  fail "Expected standalone_hello.c to be generated"
fi

# 2. Compile the C + stubs to a standalone wasm module.
cat > "$WASM_TMP/standalone_stubs.c" <<'C'
#include <stddef.h>
const char* uv_strerror(int err) { (void)err; return "uv error"; }
void emscripten_notify_memory_growth(size_t sz) { (void)sz; }
C

CC_FLAGS=(
  -I "$SI"
  -DLEAN_EMSCRIPTEN -DLEAN_WASM_COMPONENT -DLEAN_WASM_STANDALONE
  -fwasm-exceptions -s WASM_LEGACY_EXCEPTIONS=0 -s STANDALONE_WASM -s ALLOW_MEMORY_GROWTH=1
)

run_only emcc "$WASM_TMP/standalone_hello.c" -c -o "$WASM_TMP/standalone_hello.o" "${CC_FLAGS[@]}"
check_exit_is_success

run_only emcc "$WASM_TMP/standalone_stubs.c" -c -o "$WASM_TMP/standalone_stubs.o" "${CC_FLAGS[@]}"
check_exit_is_success

run_only em++ "$WASM_TMP/standalone_hello.o" "$WASM_TMP/standalone_stubs.o" \
  "$SL/libleanrt.a" "$SL/libleancpp.a" "$SL/libInit.a" \
  -o "$WASM_TMP/standalone_hello.wasm" "${CC_FLAGS[@]}" \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
check_exit_is_success

# 3. Run in wasmtime.
OUT=$("$WASMTIME" run -W exceptions=y "$WASM_TMP/standalone_hello.wasm" 2>&1) || true

if echo "$OUT" | grep -q "hello from lean"; then
  echo "PASS: standalone wasm prints 'hello from lean' in wasmtime"
else
  fail "wasmtime output does not contain 'hello from lean': $OUT"
fi

rm -f "$WASM_TMP/standalone_hello.c" "$WASM_TMP/standalone_hello.o" \
      "$WASM_TMP/standalone_stubs.o" "$WASM_TMP/standalone_hello.wasm"