#!/usr/bin/env bash
# Test: C code generation references correct extern symbols.
set -eu

LEAN_FILE="$WASM_TMP/extern_codegen.lean"
C_FILE="$WASM_TMP/extern_codegen.c"
cat > "$LEAN_FILE" <<'LEAN'
def main : IO Unit := IO.println "compiled"
LEAN

"${LEAN_CMD[@]}" -c "$C_FILE" "$LEAN_FILE" || fail "Failed to compile Lean to C"

if [ ! -f "$C_FILE" ]; then
  fail "C file was not generated"
fi

if ! grep -q "lean_get_stdout" "$C_FILE"; then
  fail "Generated C does not reference lean_get_stdout"
fi

if ! grep -q "lean_string_push" "$C_FILE"; then
  fail "Generated C does not reference lean_string_push"
fi

rm -f "$LEAN_FILE" "$C_FILE"
echo "PASS: C output contains correct extern references"