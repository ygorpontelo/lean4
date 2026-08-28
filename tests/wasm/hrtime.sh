#!/usr/bin/env bash
# Test: hrtime is monotonically increasing.
set -eu

LEAN_FILE="$WASM_TMP/hrtime.lean"
cat > "$LEAN_FILE" <<'LEAN'
import Std.Internal.UV.System
open Std.Internal.UV.System

#eval do
  let t1 ← hrtime
  let _ := List.range 1000 |>.map Nat.succ
  let t2 ← hrtime
  IO.println s!"{t1} {t2}"
LEAN

OUT=$("${LEAN_CMD[@]}" -Dlinter.all=false "$LEAN_FILE" 2>/dev/null || true)

T1=$(echo "$OUT" | awk '{print $1}')
T2=$(echo "$OUT" | awk '{print $2}')

if [ -z "$T1" ] || [ -z "$T2" ]; then
  fail "hrtime did not produce two values: $OUT"
fi

if [ "$T1" -ge "$T2" ]; then
  fail "hrtime not monotonic: t1=$T1 >= t2=$T2"
fi
rm -f "$LEAN_FILE"
echo "PASS: hrtime t1=$T1 < t2=$T2"