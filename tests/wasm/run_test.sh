source "$TEST_DIR/util.sh"

# WASM tests run via `node lean.js` instead of the native `lean` binary.
# lean.js requires input files to be within the working directory (root).
LEAN_JS="$BUILD_DIR/bin/lean.js"

if [ ! -f "$LEAN_JS" ]; then
  fail "lean.js not found at $LEAN_JS"
fi

LEAN_CMD=(node "$LEAN_JS")

case "$1" in
  *.lean)
    BASE="${1%.lean}"

    if [ -f "$1.no_compile" ]; then
      # Interpret-only test: run the file and compare output
      echo "Interpreting $1"
      capture_only "$1" "${LEAN_CMD[@]}" -Dlinter.all=false "$1"
      check_out_file
      check_exit_is_success
    elif [ -f "$1.compile_only" ]; then
      # Compile-only test: generate C and check it exists
      echo "Compiling $1"
      run_only "${LEAN_CMD[@]}" -c "$BASE.c" "$1"
      check_exit_is_success
      if [ ! -f "$BASE.c" ]; then
        fail "Expected $BASE.c to be generated"
      fi
      rm -f "$BASE.c"
    else
      # Default: run the file and compare output
      echo "Running $1"
      capture_only "$1" "${LEAN_CMD[@]}" -Dlinter.all=false "$1"
      check_out_file
      check_exit_is_success
    fi
    ;;
  *.sh)
    # Shell-based test: source the script.
    # LEAN_CMD and LEAN_JS are available.
    # Temp files must be within the working directory (wasm root constraint).
    export LEAN_JS
    WASM_TMP="$PWD"
    source "$1"
    ;;
  *)
    fail "Unknown test file type: $1"
    ;;
esac