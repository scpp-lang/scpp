# SCPP26 Language Standard

**Status**: Working draft. Incomplete -- clauses are added incrementally;
absence of a clause on some topic means this document does not yet modify
the C++ standard's treatment of that topic, not that SCPP26 has no rule
for it (see [the book](../../book/README.md) for the topics not yet
formalized here).

---

## 1 Scope

(1) This document specifies the form and establishes the interpretation
of programs written in the SCPP26 programming language.

(2) SCPP26 is defined as ISO/IEC 14882:2026, *Programming languages —
C++* (hereafter "the C++ standard" or "C++26"), modified as specified by
the following clauses. Except as explicitly modified by a later clause
of this document, every requirement of the C++ standard applies to a
SCPP26 program unchanged.

(3) This document identifies SCPP26's departure from the C++ standard by
difference only: the small number of new syntactic forms it introduces
(see Clause 5 for the first), the additional semantic restrictions and
static checks it imposes on programs the C++ standard would otherwise
accept without further constraint, and, where a later clause states so
explicitly, constructs the C++ standard permits that this document does
not.

(4) Conformance to this document requires conformance to the C++
standard as modified herein; this document does not by itself define a
complete programming language.

## 2 Normative references

(1) The following referenced document is indispensable for the
application of this document: ISO/IEC 14882:2026, *Programming
languages — C++*.

(2) For undated references, the latest edition of the referenced
document (including any amendments) applies.

## 3 Terms and definitions

For the purposes of this document, the terms and definitions given in
the C++ standard apply, together with the following.

**3.1 erasure**
the source-level transformation of a SCPP26 translation unit obtained
by, in order:
  (3.1.1) replacing every *unsafe-compound-statement* ([§5](01-unsafe.md#52-statements-stmtunsafe)) with its own
  contained *compound-statement*, discarding the `unsafe` keyword itself;
  (3.1.2) removing every attribute whose *attribute-namespace*
  ([dcl.attr.grammar]) is `scpp`.

[Note: Clause 4 requires the result to be a well-formed C++26
translation unit. — end note]

**3.2 safe context**
any point in a SCPP26 program not lexically enclosed by the
*compound-statement* of an *unsafe-compound-statement* ([§5](01-unsafe.md#52-statements-stmtunsafe)).

**3.3 unsafe context**
any point in a SCPP26 program lexically enclosed by the
*compound-statement* of an *unsafe-compound-statement* ([§5](01-unsafe.md#52-statements-stmtunsafe)).

**3.4 gated operation**
an operation this document identifies as ill-formed in a safe context
(3.2) and well-formed in an unsafe context (3.3).

## 4 Conformance

(1) A conforming implementation shall issue at least one diagnostic
message for every SCPP26 translation unit that is ill-formed under this
document or under the C++ standard as modified by this document, except
where this document explicitly permits an implementation to forgo
diagnosing a specific ill-formed construct.

(2) A conforming implementation shall accept the erasure (3.1) of every
well-formed SCPP26 translation unit as a well-formed C++26 translation
unit.

[Note: this document may require an implementation to perform an
additional runtime check (for example, a future clause's
arithmetic-overflow check) that C++26 does not require when translating
the same erased text; the observable behavior of the two need therefore
not coincide for an execution that would trigger such a check. — end
note]

---

[Table of Contents](README.md) · [Next: The `unsafe` Compound Statement →](01-unsafe.md)
