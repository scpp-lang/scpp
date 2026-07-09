# The SCPP Programming Language

Welcome to *The SCPP Programming Language*.

scpp is trying to do something unusual: keep the surface feel of modern C++,
while making Rust-style ownership, borrowing, and lifetime checking the default.
This book teaches that idea as a language you can actually write, compile, and
run today.

## Who this book is for

This book is primarily for readers who already recognize ordinary C++ syntax and
want to understand what scpp changes semantically.

You do **not** need to be a compiler engineer, and you do **not** need Rust
experience. If you already know what a function, a variable, and a compiler are,
the early chapters will walk you through the rest with working programs.

## How to read this book

The book has two layers:

- the **tutorial path** at the front walks through real programs first and adds
  rules only when they become useful;
- the later chapters keep design notes, roadmap material, and reference-heavy
  details that are still useful once you want the fuller picture.

For precise, normative wording, use the formal specification in `docs/spec/`.
中文版: [zh/README.md](../zh/README.md)

- Tutorial Path
  - [Getting Started](ch00-design-philosophy.md)
  - [A Small Complete Program](ch01-safety-context.md)
  - [Basic Building Blocks](ch02-boundary-rules.md)
  - [Borrowing, Moving, and Views Behind Familiar Syntax](ch03-syntactic-sugar.md)
  - [Struct vs Class Semantics (Fixed Memory Layout / ABI)](ch04-struct-vs-class.md)
  - [Static Checks (the soundness core)](ch05-static-checks.md)
- Design Notes and Reference
  - [The v0.1 Supported Subset](ch06-safe-subset.md)
  - [Compilation Pipeline (architecture)](ch07-compilation-pipeline.md)
  - [Open Questions (to be decided later)](ch08-open-questions.md)
  - [MVP Milestones (implementation order, end-to-end first)](ch09-milestones.md)
  - [Reference Implementations (required reading)](ch10-reference-implementations.md)
  - [Modules & Libraries](ch11-modules-and-libraries.md)
  - [IDE Integration](ch12-ide-integration.md)
  - [Compiler Invocation and CLI](ch13-compiler-invocation.md)
