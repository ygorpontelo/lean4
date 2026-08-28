#!/usr/bin/env bash
# Test: osUname returns four non-empty fields.
set -eu

LEAN_FILE="$WASM_TMP/uname.lean"
cat > "$LEAN_FILE" <<'LEAN'
import Std.Internal.UV.System
open Std.Internal.UV.System

#eval do
  let u ← osUname
  IO.println u.sysname
  IO.println u.release
  IO.println u.version
  IO.println u.machine
LEAN

OUT=$("${LEAN_CMD[@]}" -Dlinter.all=false "$LEAN_FILE" 2>/dev/null || true)

SYSNAME=$(echo "$OUT" | sed -n 1p)
RELEASE=$(echo "$OUT" | sed -n 2p)
VERSION=$(echo "$OUT" | sed -n 3p)
MACHINE=$(echo "$OUT" | sed -n 4p)

for FIELD in "$SYSNAME" "$RELEASE" "$VERSION" "$MACHINE"; do
  if [ -z "$FIELD" ]; then
    fail "osUname returned an empty field"
  fi
done
rm -f "$LEAN_FILE"
echo "PASS: uname sys=$SYSNAME rel=$RELEASE mach=$MACHINE"