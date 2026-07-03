# The SCPP Programming Language

*Language specification draft v0.1 (codename TBD)*

> A language that "looks exactly like idiomatic modern C++", adding only a very
> small set of extensions (the core being the `safe` keyword). Regions annotated
> with `safe` enable Rust-style sound compile-time safety checks; all other code
> follows ordinary C++ semantics. The backend generates native binaries via LLVM.

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是 `safe`
> 关键字）。被 `safe` 标注的区域启用 Rust 式健全的编译期安全检查；其余代码按
> 普通 C++ 语义处理。后端经 LLVM 生成本地二进制。
>
> 中文版: [zh/README.md](../zh/README.md)

*Status: draft v0.1 · pending review before entering M1.*

## Table of Contents

0. [Design Philosophy](ch00-design-philosophy.md)
1. [Safety Context](ch01-safety-context.md)
2. [Boundary Rules (Safe <-> Unsafe interaction)](ch02-boundary-rules.md)
3. [Syntactic Sugar / Re-semantification of Existing Syntax](ch03-syntactic-sugar.md)
4. [Struct vs Class Semantics (Fixed Memory Layout / ABI)](ch04-struct-vs-class.md)
5. [Static Checks in Safe Regions (the soundness core)](ch05-static-checks.md)
6. [The Safe Subset Supported in v0.1](ch06-safe-subset.md)
7. [Compilation Pipeline (architecture)](ch07-compilation-pipeline.md)
8. [Open Questions (to be decided later)](ch08-open-questions.md)
9. [MVP Milestones (implementation order, end-to-end first)](ch09-milestones.md)
10. [Reference Implementations (required reading)](ch10-reference-implementations.md)
