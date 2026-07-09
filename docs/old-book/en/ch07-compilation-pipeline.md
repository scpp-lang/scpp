# 7. Compilation Pipeline (architecture)

```
source
 └─► Lexer
     └─► recursive-descent Parser ──► unified AST (carries an unsafe-block flag on nodes)
         └─► name resolution + type checking ──► HIR (desugar: std::move -> move hint, etc.)
             └─► MIR (CFG + three-address)
                 └─► borrow check (init / move / alias / lifetime -- runs
                     unconditionally; the unsafe-nesting counter only
                     relaxes the specific §5.5 operations within this pass,
                     never skips it)
                     └─► lower after checks pass
                          └─► LLVM IR ──► LLVM opt ──► target binary
```

Key points:
- **Unified AST**: every function shares one AST shape, with an
  unsafe-block flag on nodes marking which statements are lexically
  inside a `[[scpp::unsafe]] { }` compound-statement
  ([§1](ch01-safety-context.md)); the parser sets that flag while
  handling the statement's ordinary attribute-specifier-seq, not via a
  distinct `unsafe` grammar production. A function whose own declaration
  carries the same attribute has that flag set on every statement in its
  body directly (see [§1.2](ch01-safety-context.md)), and the function's
  `FunctionDecl` node itself carries a separate flag the call-checking
  pass consults to gate calls to it, exactly like the existing `extern
  "C"` check.
- **Borrow checking runs on every function's MIR, unconditionally** --
  `[[scpp::unsafe]] { }` never skips this pass; it only relaxes the fixed,
  enumerated operations in [§5.5](ch05-static-checks.md) *within* it (raw
  pointer dereference, calling an `extern "C"` function, etc.), exactly
  mirroring how Rust's own borrow checker keeps running inside an
  `unsafe fn`/`unsafe { }` block.
- The frontend only needs to handle the constructs in
  [§6](ch06-safe-subset.md)'s supported subset -- full C++ standard
  compliance for arbitrary, unsupported constructs is not pursued.
- MIR makes things explicit: ownership transfers, borrow start/end, drop
  insertion points, CFG.
- This diagram is the pipeline for a **single file**.
  [ch11](ch11-modules-and-libraries.md) specifies how an
  `import`ed module's signatures/struct layouts get fed into the "name
  resolution + type checking" step above for a multi-file program --
  the pipeline itself doesn't change, it's just seeded from more than one
  source.
- **Why lower directly to LLVM IR, not transpile to C++ text and let
  Clang do it**: the latter was seriously considered specifically to get
  arbitrary existing-C++-library interop almost for free (real templates,
  classes, exceptions, RTTI, all handled by a real C++ compiler). Rejected
  because (a) it doesn't make the checked pipeline above any
  simpler or smaller -- borrow-checking work is identical either way, so the
  cost of a full codegen rework wasn't justified by that alone; and
  (b) it has a strictly *lower* optimization ceiling than direct IR
  generation: LLVM's `noalias`/`alias.scope`
  metadata can express aliasing facts scoped to a sub-region of a
  function's body, derived from the borrow checker's own NLL-precision
  analysis, and no C++ source-level construct (not even `__restrict`,
  which only ever maps to the coarser, whole-parameter-lifetime `noalias`
  *attribute*) can reach that finer-grained metadata. `extern "C"`
  ([§2.1](ch02-boundary-rules.md)) remains scpp's sole, deliberately
  narrow interop mechanism with the outside world (see
  [§8](ch08-open-questions.md) item 6).

---

[← Previous: The v0.1 Supported Subset](ch06-safe-subset.md) · [Table of Contents](README.md) · [Next: Open Questions →](ch08-open-questions.md)
