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

---

[← Previous: The Safe Subset Supported in v0.1](ch06-safe-subset.md) · [Table of Contents](README.md) · [Next: Open Questions →](ch08-open-questions.md)
