#!/usr/bin/env bash
# Test: cwd matches the real working directory.
set -eu

LEAN_FILE="$WASM_TMP/cwd.lean"
cat > "$LEAN_FILE" <<'LEAN'
import Std.Internal.UV.System
open Std.Internal.UV.System

#eval do
  let cwd ← Std.Internal.UV.System.cwd
  IO.println cwd
LEAN

LEAN_CWD=$("${LEAN_CMD[@]}" -Dlinter.all=false "$LEAN_FILE" 2>/dev/null | head -1 || true)

NODE_CWD=$(node -e "process.stdout.write(process.cwd())")

if [ "$LEAN_CWD" != "$NODE_CWD" ]; then
  fail "Lean cwd ($LEAN_CWD) != Node cwd ($NODE_CWD)"
fi
rm -f "$LEAN_FILE"
echo "PASS: cwd = $LEAN_CWD"