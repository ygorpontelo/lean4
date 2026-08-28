#!/usr/bin/env bash
# Test: osSetenv / osGetenv / osUnsetenv round-trip.
set -eu

LEAN_FILE="$WASM_TMP/getenv.lean"
cat > "$LEAN_FILE" <<'LEAN'
import Std.Internal.UV.System
open Std.Internal.UV.System

#eval do
  osSetenv "LEAN_WASM_TEST_VAR" "hello123"
  let v1 ← osGetenv "LEAN_WASM_TEST_VAR"
  IO.println (toString v1)
  osUnsetenv "LEAN_WASM_TEST_VAR"
  let v2 ← osGetenv "LEAN_WASM_TEST_VAR"
  IO.println (toString v2)
LEAN

OUT=$("${LEAN_CMD[@]}" -Dlinter.all=false "$LEAN_FILE" 2>/dev/null || true)

LINE1=$(echo "$OUT" | sed -n 1p)
LINE2=$(echo "$OUT" | sed -n 2p)

if [ "$LINE1" != "(some hello123)" ]; then
  fail "osGetenv after osSetenv returned: $LINE1, expected: (some hello123)"
fi
if [ "$LINE2" != "none" ]; then
  fail "osGetenv after osUnsetenv returned: $LINE2, expected: none"
fi
rm -f "$LEAN_FILE"
echo "PASS: getenv round-trip works"