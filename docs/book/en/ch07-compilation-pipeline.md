# 7. Compilation Pipeline (architecture)

```
source
 └─► Lexer
     └─► recursive-descent Parser ──► unified AST (one AST for safe/unsafe, carries a safety tag)
         └─► name resolution + type checking ──► HIR (desugar: std::move -> move hint, etc.)
             ├─ [unsafe region] ─────────────────────────► lower directly
             └─ [safe region] ─► MIR (CFG + three-address)
                            └─► borrow check (init / move / alias / lifetime)
                                └─► lower after checks pass
                 └─► LLVM IR ──► LLVM opt ──► target binary
```

Key points:
- **Unified AST**: safe and unsafe code share one AST, with a safety-context bit
  on nodes.
- **Borrow checking runs only on the MIR of safe regions**; unsafe regions skip
  it and lower directly.
- The frontend need only be "good enough" for unsafe/ordinary C++ — full C++
  compatibility is not pursued.
- MIR makes things explicit: ownership transfers, borrow start/end, drop
  insertion points, CFG.
- This diagram is the pipeline for **one file** (today, the only kind
  there is). [ch11](ch11-modules-and-libraries.md) specifies how an
  `import`ed module's signatures/struct layouts get fed into the "name
  resolution + type checking" step above for a multi-file program --
  the pipeline itself doesn't change, it's just seeded from more than one
  source.
- **Why lower directly to LLVM IR, not transpile to C++ text and let
  Clang do it**: the latter was seriously considered specifically to get
  arbitrary existing-C++-library interop almost for free (real templates,
  classes, exceptions, RTTI, all handled by a real C++ compiler). Rejected
  because (a) it doesn't make the checked-region pipeline above any
  simpler or smaller -- movecheck's work is identical either way, so the
  cost of a full codegen rework wasn't justified by that alone; and
  (b) it has a strictly *lower* optimization ceiling than direct IR
  generation for safe-region code: LLVM's `noalias`/`alias.scope`
  metadata can express aliasing facts scoped to a sub-region of a
  function's body, derived from the borrow checker's own NLL-precision
  analysis, and no C++ source-level construct (not even `__restrict`,
  which only ever maps to the coarser, whole-parameter-lifetime `noalias`
  *attribute*) can reach that finer-grained metadata. `extern "C"`
  ([§2.1](ch02-boundary-rules.md)) remains scpp's sole, deliberately
  narrow interop mechanism with the outside world (see
  [§8](ch08-open-questions.md) item 6).

---

[← Previous: The Safe Subset Supported in v0.1](ch06-safe-subset.md) · [Table of Contents](README.md) · [Next: Open Questions →](ch08-open-questions.md)
