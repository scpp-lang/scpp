# 5 The `unsafe` Compound Statement

## 5.1 Keywords [lex.key]

(1) `unsafe` is a keyword. It is reserved in every context; it shall not
be used as an identifier.

[Note: this document does not add `unsafe` as a context-sensitive
identifier along the lines of C++26's `override`, `final`, `import`, or
`module` ([lex.name]); a program that uses `unsafe` as the name of an
entity, however that entity's own well-formedness would otherwise be
judged, is ill-formed. — end note]

## 5.2 Statements [stmt.unsafe]

(1) Syntax:

```
statement:
    ...
    unsafe-compound-statement

unsafe-compound-statement:
    unsafe compound-statement
```

(2) The *compound-statement* of an *unsafe-compound-statement* is an
ordinary *compound-statement* ([stmt.block]): it introduces a block
scope exactly as any other *compound-statement* does, and every name
declared within it obeys the same scoping rules as in any other block.

[Note: an *unsafe-compound-statement* is not itself a distinct kind of
scope. This document does not give `unsafe { }` any scoping behavior
different from an ordinary `{ }` compound statement. — end note]

(3) The *compound-statement* of an *unsafe-compound-statement* is an
unsafe context ([§3.3](00-front-matter.md#3-terms-and-definitions));
every other point in the program is a safe context
([§3.2](00-front-matter.md#3-terms-and-definitions)).

(4) An *unsafe-compound-statement* may appear lexically nested within
the *compound-statement* of another *unsafe-compound-statement*. Such
nesting has no effect: the nested and enclosing *compound-statement*s
are both unsafe contexts, as (3) already requires independently of any
nesting.

(5) The following are gated operations
([§3.4](00-front-matter.md#3-terms-and-definitions)):

  (5.1) indirection through, or pointer arithmetic on, a value of
  pointer type ([expr.unary.op], [expr.add]);

  (5.2) `reinterpret_cast` ([expr.reinterpret.cast]), and any
  explicit-type-conversion ([expr.cast]) between two pointer types
  neither of which is convertible to the other by an implicit
  conversion this document permits;

  (5.3) access to a non-static data member of a union that is not a
  tagged union (a future clause defines *tagged union* for SCPP26;
  until such a clause is added to this document, every union is
  untagged for this purpose) ([class.union]);

  (5.4) a *new-expression* or a *delete-expression* ([expr.new],
  [expr.delete]);

  (5.5) an lvalue-to-rvalue conversion of, or an assignment to, a
  variable of static or thread storage duration that is not
  const-qualified ([basic.stc.static], [basic.stc.thread]);

  (5.6) a function call whose *postfix-expression* denotes a function
  declared with C language linkage ([dcl.link]).

(6) Except as this document explicitly states otherwise, a gated
operation (5) is ill-formed in a safe context and well-formed in an
unsafe context.

(7) This document imposes requirements on a program -- including, but
not limited to, requirements on ownership, aliasing, lifetime, and
arithmetic overflow -- in clauses other than this one. Except where
such a clause explicitly says otherwise, that clause's requirements
apply identically whether the construct they govern appears in a safe
context or an unsafe context: an *unsafe-compound-statement* relaxes
only the ill-formedness described in (6), for the gated operations
enumerated in (5), and nothing else.

[Note: in particular, a future clause that requires an implementation to
perform a runtime check (for example, on arithmetic overflow, or on an
out-of-bounds index) may, unlike (6), permit an implementation to skip
the check itself inside an unsafe context, while still requiring the
checked operation to be well-formed in every context. Skipping such a
check is a distinct permission from (6)'s well-formedness rule, granted
(if at all) by the clause that introduces the check, not by this
clause. — end note]

---

[← Previous: Front Matter](00-front-matter.md) · [Table of Contents](README.md) · [Next: Ownership, Initialization, and Move →](02-ownership-and-move.md)
