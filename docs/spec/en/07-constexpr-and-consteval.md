# 9 The `constexpr` and `consteval` specifiers

## 9.1 The `constexpr` and `consteval` specifiers [dcl.constexpr]

(1) Except as modified by this clause, [dcl.constexpr] applies unchanged
to a SCPP26 program.

(2) The tokens `constexpr` and `consteval` have the meanings assigned by
the C++ standard; this document introduces no additional spelling for
compile-time-evaluable declarations.

(3) A function or constructor declared `constexpr` is eligible for
compile-time evaluation. If an invocation of such a function or
constructor occurs in required constant evaluation (7.1), the invocation
shall satisfy Clause 7. Otherwise, the invocation is an ordinary runtime
invocation with the semantics the C++ standard gives it.

(4) A function or constructor declared `consteval` is an immediate
function. Every potentially-evaluated call to it shall produce a constant
expression. If it does not, the program is ill-formed.

(5) A destructor, allocation function, or deallocation function shall not
be declared `consteval`.

(6) A `constexpr` or `consteval` constructor may participate in required
constant evaluation only for a class type satisfying 7.2(2).

[Note: this clause reuses the C++ spellings and meanings of
`constexpr` and `consteval`. The SCPP26-specific restrictions are those
of Clause 7's supported constant-evaluation subset. — end note]

---

[← Previous: Constant evaluation](06-constant-evaluation.md) · [Table of Contents](README.md) · [Next: Function template argument deduction →](08-function-template-argument-deduction.md)
