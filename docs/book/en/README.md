# The SCPP Programming Language

*Language specification draft v0.1 (codename TBD)*

> A language that "looks exactly like idiomatic modern C++", adding zero new
> keywords and only a very small set of extensions spelled as attributes in
> the `scpp` namespace (the core one being `[[scpp::unsafe]]`). Every
> function is checked with Rust-style sound compile-time safety checks by
> default; `[[scpp::unsafe]] { }` blocks locally relax a fixed, narrow set of
> operations. The backend generates native binaries via LLVM.

> 一门"看起来就是原汁原味现代 C++"的语言，不新增关键字，只加入极少量、
> 以 `scpp` 命名空间属性拼写的扩展（核心是 `[[scpp::unsafe]]`）。每个函数
> 默认都启用 Rust 式健全的编译期安全检查；`[[scpp::unsafe]] { }` 块局部
> 放宽一份固定、狭窄的操作清单。后端经 LLVM 生成本地二进制。
>
> 中文版: [zh/README.md](../zh/README.md)

*Status: draft v0.1 · pending review before entering M1.*

## Table of Contents

0. [Design Philosophy](ch00-design-philosophy.md)
1. [Safety Context](ch01-safety-context.md)
2. [Boundary Rules (Interaction with `[[scpp::unsafe]] { }` and `extern "C"`)](ch02-boundary-rules.md)
3. [Syntactic Sugar / Re-semantification of Existing Syntax](ch03-syntactic-sugar.md)
4. [Struct vs Class Semantics (Fixed Memory Layout / ABI)](ch04-struct-vs-class.md)
5. [Static Checks (the soundness core)](ch05-static-checks.md)
6. [The v0.1 Supported Subset](ch06-safe-subset.md)
7. [Compilation Pipeline (architecture)](ch07-compilation-pipeline.md)
8. [Open Questions (to be decided later)](ch08-open-questions.md)
9. [MVP Milestones (implementation order, end-to-end first)](ch09-milestones.md)
10. [Reference Implementations (required reading)](ch10-reference-implementations.md)
11. [Modules & Libraries](ch11-modules-and-libraries.md)
12. [IDE Integration](ch12-ide-integration.md)
13. [Compiler Invocation and CLI](ch13-compiler-invocation.md)
