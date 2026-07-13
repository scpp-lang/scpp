# SCPP26 Specifications

*Formal specifications -- working drafts.*

> This directory contains three side-by-side SCPP26 specifications. The first
> is the language standard itself: an ISO-style list of clauses describing how
> SCPP26 modifies C++26. The other two are standalone file-format
> specifications for `.scppm` and `.scppkg`.
>
> 中文版: [zh/README.md](../zh/README.md)

## Language Standard

This document specifies SCPP26 as ISO/IEC 14882:2026 (C++26), modified by the
numbered clauses below. It is a companion to
[the book](../../book/en/README.md), which teaches the language; this document
instead states, precisely and normatively, exactly what changes relative to the
C++ standard. Clauses are added incrementally; a topic with no clause here yet
is still designed (see the book) but not yet formalized in this document.

### Table of Contents

0. [Front Matter (Scope, Normative References, Terms and Definitions, Conformance)](00-front-matter.md)
5. [The `[[scpp::unsafe]]` Attribute](01-unsafe.md)
6. [Ownership, Initialization, and Move](02-ownership-and-move.md)
7. [Dereference and Member Access](03-dereference-and-member-access.md)
8. [Thread-Safety Properties](04-thread-safety-properties.md)
9. [Union types and packed layout](05-unions-and-packed-layout.md)
7. [Constant evaluation](06-constant-evaluation.md)
9. [The `constexpr` and `consteval` specifiers](07-constexpr-and-consteval.md)
13. [Function template argument deduction](08-function-template-argument-deduction.md)
14. [Enumeration conversions](09-enumeration-conversions.md)
8. [Iteration statements](10-iteration-statements.md)

## File-Format Specifications

- [The `.scppm` Module Interface Format](scppm-format.md)
- [The `.scppkg` Package Format](scppkg-format.md)
