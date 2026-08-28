/-
Copyright (c) 2022 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Leonardo de Moura
-/
module

prelude
public import Lean.Util.Trace

public section

namespace Lean.Compiler

register_builtin_option compiler.check : Bool := {
  defValue := false
  descr    := "type check code after each compiler step (this is useful for debugging purses)"
}

register_builtin_option compiler.traceUnnormalized : Bool := {
  defValue := false
  descr    := "don't normalize declarations before tracing them at each pipeline step (this is \
    useful for debugging purposes)"
}

register_builtin_option compiler.checkMeta : Bool := {
  defValue := true
  descr := "Check that `meta` declarations only refer to other `meta` declarations and ditto for \
    non-`meta` declarations. Disabling this option may lead to delayed compiler errors and is
    intended only for debugging purposes."
}

register_builtin_option compiler.relaxedMetaCheck : Bool := {
  defValue := false
  descr := "Allow mixed `meta`/non-`meta` references in the same module. References to imports are unaffected."
}

register_builtin_option compiler.ignoreBorrowAnnotation : Bool := {
  defValue := false
  descr := "Ignore user defined borrow inference annotations. This is useful for export/extern \
    forward declarations"
}

register_builtin_option compiler.postponeCompile : Bool := {
  defValue := false
  descr := "Internal. Toggle experimental `leanir` separate compilation."
}

register_builtin_option compiler.inLeanIR : Bool := {
  defValue := false
  descr := "Internal. Indicates whether the compiler is currently running in `leanir`."
}

register_builtin_option compiler.target.ptrWidth : Nat := {
  defValue := System.Platform.numBits
  descr := "(compiler) target pointer width in bits (32 or 64) for cross-compilation. Defaults to the host width."
}

/-- The exclusive upper bound for tagged `Nat` values at the target pointer width: `2^(ptrWidth-1)`.
Readable from `CoreM` without depending on `Lean.Compiler.LCNF.ConfigOptions`. This duplicates
`LCNF.ConfigOptions.smallNatThreshold`; the two must stay in sync (same formula, same option key). -/
def targetSmallNatThreshold (opts : Options) : Nat :=
  2 ^ (compiler.target.ptrWidth.get opts - 1)

end Lean.Compiler
