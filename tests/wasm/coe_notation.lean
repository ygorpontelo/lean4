-- Test: data symbol lookup via WebAssembly.Global doesn't crash.
-- The ↑ coercion notation triggers lookup of l_coeNotation, which is a
-- syntax declaration exported as a WebAssembly.Global (a data value),
-- not a function. The interpreter must return the global's numeric value
-- instead of inserting it into the indirect function table.

structure Box (α : Type) where
  val : α

instance : Coe (Box α) α where
  coe x := x.val

def unwrap (x : Box Nat) : Nat := ↑x

#eval unwrap { val := 42 }