#!/usr/bin/env bash
# Test: osGetPid returns a valid positive integer.
set -eu

LEAN_FILE="$WASM_TMP/getpid.lean"
cat > "$LEAN_FILE" <<'LEAN'
import Std.Internal.UV.System
open Std.Internal.UV.System

#eval do
  let pid ← osGetPid
  IO.println (toString pid)
LEAN

OUT=$("${LEAN_CMD[@]}" -Dlinter.all=false "$LEAN_FILE" 2>/dev/null || true)

PID=$(echo "$OUT" | head -1)
if ! echo "$PID" | grep -qP '^\d+$'; then
  fail "osGetPid did not return a number: $OUT"
fi
if [ "$PID" -le 0 ]; then
  fail "osGetPid returned non-positive PID: $PID"
fi
rm -f "$LEAN_FILE"
echo "PASS: osGetPid = $PID"