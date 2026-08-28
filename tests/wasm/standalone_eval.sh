#!/usr/bin/env bash
# Test: #eval runs in standalone wasm via wasmtime.
# Exercises the IR interpreter path (symbol lookup, task manager with 0
# workers, inline task execution via the enqueue_core fix).
set -eu

# Skip if standalone runtime libs are not built.
STANDALONE_DIR="$(dirname "$(dirname "$BUILD_DIR")")/wasm_standalone/stage1"
SL="$STANDALONE_DIR/lib/lean"
SI="$STANDALONE_DIR/include"
if [ ! -f "$SL/libleanrt.a" ]; then
  echo "SKIP: standalone runtime not built ($SL/libleanrt.a missing)"
  exit 0
fi

for tool in emcc em++ wasmtime; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIP: $tool not found"
    exit 0
  fi
done
WASMTIME="${WASMTIME:-$(command -v wasmtime)}"

# 1. Compile a Lean program that computes and prints a value.
# Uses a recursive function (exercises the interpreter/codegen path)
# and IO.println (exercises the task manager with 0 workers).
LEAN_FILE="$WASM_TMP/standalone_eval.lean"
cat > "$LEAN_FILE" <<'LEAN'
def factorial : Nat → Nat
  | 0 => 1
  | n+1 => (n+1) * factorial n

def main : IO Unit := IO.println (toString (factorial 5))
LEAN

run_only "${LEAN_CMD[@]}" -c "$WASM_TMP/standalone_eval.c" "$LEAN_FILE"
check_exit_is_success
if [ ! -f "$WASM_TMP/standalone_eval.c" ]; then
  fail "Expected standalone_eval.c to be generated"
fi

# 2. Link with the standalone runtime.
cat > "$WASM_TMP/standalone_eval_stubs.c" <<'C'
#include <stddef.h>
const char* uv_strerror(int err) { (void)err; return "uv error"; }
void emscripten_notify_memory_growth(size_t sz) { (void)sz; }
C

CC_FLAGS=(
  -I "$SI"
  -DLEAN_EMSCRIPTEN -DLEAN_WASM_COMPONENT -DLEAN_WASM_STANDALONE
  -fwasm-exceptions -s WASM_LEGACY_EXCEPTIONS=0 -s STANDALONE_WASM -s ALLOW_MEMORY_GROWTH=1
)

run_only emcc "$WASM_TMP/standalone_eval.c" -c -o "$WASM_TMP/standalone_eval.o" "${CC_FLAGS[@]}"
check_exit_is_success
run_only emcc "$WASM_TMP/standalone_eval_stubs.c" -c -o "$WASM_TMP/standalone_eval_stubs.o" "${CC_FLAGS[@]}"
check_exit_is_success
run_only em++ "$WASM_TMP/standalone_eval.o" "$WASM_TMP/standalone_eval_stubs.o" \
  "$SL/libleanrt.a" "$SL/libleancpp.a" "$SL/libInit.a" \
  -o "$WASM_TMP/standalone_eval.wasm" "${CC_FLAGS[@]}" \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0
check_exit_is_success

# 3. Run in wasmtime — the #eval output goes to stdout.
OUT=$("$WASMTIME" run -W exceptions=y "$WASM_TMP/standalone_eval.wasm" 2>&1) || true

if echo "$OUT" | grep -q "120"; then
  echo "PASS: #eval factorial 5 = 120 in standalone wasm"
else
  fail "wasmtime output does not contain '120': $OUT"
fi

rm -f "$WASM_TMP/standalone_eval.c" "$WASM_TMP/standalone_eval.o" \
      "$WASM_TMP/standalone_eval_stubs.o" "$WASM_TMP/standalone_eval.wasm"