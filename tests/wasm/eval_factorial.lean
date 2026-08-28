-- Test: end-to-end evaluation pipeline.
-- #eval exercises the full symbol lookup → interpreter → evaluation
-- pipeline. Any symbol lookup failure crashes the compiler.

def factorial : Nat → Nat
  | 0 => 1
  | n+1 => (n+1) * factorial n

#eval factorial 5