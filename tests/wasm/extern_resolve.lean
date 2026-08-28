-- Test: function symbol lookup resolves correctly.
-- IO.println calls runtime functions via extern (lean_get_stdout,
-- lean_string_push, etc.). If wasmTable.set returns a wrong index,
-- call_indirect calls the wrong function or traps.

#eval IO.println "hello"
#eval IO.println "world"
#eval IO.println s!"{1 + 1}"