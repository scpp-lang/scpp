# 5 The `[[scpp::unsafe]]` Attribute

## 5.1 Attributes [dcl.attr.scpp.unsafe]

(1) The *attribute-token* `unsafe`, in the *attribute-namespace* `scpp`
([dcl.attr.grammar]), may be applied to:

  (1.1) a *compound-statement*, via that *compound-statement*'s own
  *attribute-specifier-seq* ([stmt.block]); or

  (1.2) a function, via the leading *attribute-specifier-seq* of a
  *function-definition* ([dcl.fct.def.general]) or of a declaration
  ([dcl.pre]) whose declarator denotes that function.

No *attribute-argument-clause* shall be present. If an
*attribute-specifier-seq* containing the attribute-token `unsafe`
appertains to anything other than (1.1) or (1.2), the program is
ill-formed.

[Note: this introduces no new grammar. [stmt.block] already gives every
*compound-statement* its own optional leading *attribute-specifier-seq*
(as in C++26's `[[likely]] { ... }`), and [dcl.fct.def.general]/[dcl.pre]
already give every function-definition/declaration its own optional
leading *attribute-specifier-seq* (as in C++26's `[[noreturn]] void
f();`); this subclause gives meaning to one specific attribute-token,
spelled in an attribute-namespace this document reserves for itself,
exactly as [dcl.attr.fallthrough] gives meaning to `fallthrough` and
[dcl.attr.noreturn] gives meaning to `noreturn`, neither introducing any
grammar of its own. An *attribute-specifier-seq* appearing immediately
after a function's *parameters-and-qualifiers* ([dcl.fct]) -- as
distinct from immediately before that function's *decl-specifier-seq*,
(1.2)'s position -- appertains to the function's type, not to the
function itself, and so satisfies neither (1.1) nor (1.2). — end note]

(2) If a program declares the same function more than once, and an
*attribute-specifier-seq* containing the attribute-token `unsafe`
appertains (1.2) to one such declaration, such an *attribute-specifier-seq*
shall appertain to every declaration of that function; otherwise the
program is ill-formed.

[Note: this rules out declaring a function with the attribute in one
place and calling it through a declaration that lacks the attribute
elsewhere, which would otherwise defeat (6)'s gating. — end note]

(3) A *compound-statement* to which an *attribute-specifier-seq*
containing the attribute-token `unsafe` appertains (1.1), and the entire
*function-body* ([dcl.fct.def.general]) of a function to which an
*attribute-specifier-seq* containing the attribute-token `unsafe`
appertains (1.2), are each an unsafe context
([§3.3](00-front-matter.md#3-terms-and-definitions)); every other point
in the program is a safe context
([§3.2](00-front-matter.md#3-terms-and-definitions)).

[Note: neither case is itself a distinct kind of scope. This document
does not give a *compound-statement* reached via (1.1) or (1.2) any
scoping behavior different from an ordinary *compound-statement*
([stmt.block]): it introduces a block scope exactly as any other
*compound-statement* does, and every name declared within it obeys the
same scoping rules as in any other block. — end note]

```cpp
int legacy_style_function(int* p, int n) {
    [[scpp::unsafe]] {
        // the whole body lives here
    }
}

[[scpp::unsafe]] int get_unchecked(int* base, int index) {
    return base[index];   // no nested [[scpp::unsafe]] needed here: (3)
                           // already makes the entire body an unsafe
                           // context, because of the attribute above
}
```

(4) A *compound-statement* or *function-body* that is an unsafe context
by (3) may appear lexically nested within another *compound-statement*
or *function-body* that is likewise an unsafe context by (3). Such
nesting has no effect: both remain unsafe contexts, as (3) already
requires independently of any nesting.

(5) The following are gated operations
([§3.4](00-front-matter.md#3-terms-and-definitions)):

  (5.1) indirection through, or pointer arithmetic on, a value of
  pointer type ([expr.unary.op], [expr.add]);

  (5.2) `reinterpret_cast` ([expr.reinterpret.cast]), and any
  explicit-type-conversion ([expr.cast]) between two pointer types
  neither of which is convertible to the other by an implicit
  conversion this document permits;

  (5.3) access to a non-static data member of a union
  ([class.union], as modified by
  [§9.1](05-unions-and-packed-layout.md#91-union-types));

  (5.4) a *new-expression* or a *delete-expression* ([expr.new],
  [expr.delete]);

  (5.5) an lvalue-to-rvalue conversion of, or an assignment to, a
  variable of static or thread storage duration that is not
  const-qualified ([basic.stc.static], [basic.stc.thread]);

  (5.6) a function call whose *postfix-expression* denotes a function
  declared with C language linkage ([dcl.link]);

  (5.7) a function call whose *postfix-expression* denotes a function to
  which an *attribute-specifier-seq* containing the attribute-token
  `unsafe` appertains (1.2).

(6) Except as this document explicitly states otherwise, a gated
operation (5) is ill-formed in a safe context and well-formed in an
unsafe context.

(7) This document imposes requirements on a program -- including, but
not limited to, requirements on ownership, aliasing, lifetime, and
arithmetic overflow -- in clauses other than this one. Except where such
a clause explicitly says otherwise, that clause's requirements apply
identically whether the construct they govern appears in a safe context
or an unsafe context: this clause relaxes only the ill-formedness
described in (6), for the gated operations enumerated in (5), and
nothing else -- in particular, an unsafe context reached by (3), however
it was reached, never relaxes any other clause's requirements.

[Note: in particular, a future clause that requires an implementation to
perform a runtime check (for example, on arithmetic overflow, or on an
out-of-bounds index) may, unlike (7), permit an implementation to skip
the check itself inside an unsafe context, while still requiring the
checked operation to be well-formed in every context. Skipping such a
check is a distinct permission from (7)'s well-formedness rule, granted
(if at all) by the clause that introduces the check, not by this
clause. — end note]

## 5.2 Function pointer types [dcl.ptr.scpp.unsafe]

(1) The attribute-token `unsafe`, in the *attribute-namespace* `scpp`, may
also appertain to a `*` *ptr-operator* ([dcl.ptr]) that forms a pointer to
function type, via that *ptr-operator*'s own *attribute-specifier-seq*. No
*attribute-argument-clause* shall be present.

[Note: this introduces no new grammar: [dcl.ptr] already gives every `*`
*ptr-operator* an optional *attribute-specifier-seq* of its own (as in
`int* [[maybe_unused]] p;`); this subclause gives meaning to the
attribute-token `unsafe` in that existing grammar slot, exactly as
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) gives it meaning in the
two grammar slots that subclause covers. — end note]

```cpp
int (* [[scpp::unsafe]] up)(int, int);   // pointer to an unsafe-qualified
                                          // function type
int (*                  sp)(int, int);   // pointer to a function type that
                                          // is not unsafe-qualified -- a
                                          // different type from up's, by (2)
```

(2) A pointer-to-function type to which the attribute-token `unsafe`
appertains (1) (an *unsafe-qualified* pointer-to-function type), and the
otherwise-identical pointer-to-function type to which it does not, are
distinct types.

[Note: this parallels a *noexcept-specifier*'s effect on a function type
([dcl.fct]): `void(*)()` and `void(*)() noexcept` are likewise distinct
types. In each case, one of the two types promises something about how the
pointee may be used that the other does not, and that promise is tracked as
part of the type itself. — end note]

(3) An expression consisting of the unary `&` operator applied to an
*id-expression* that designates a function ([expr.unary.op]), or an
*id-expression* designating a function converted to a prvalue of
pointer-to-function type ([conv.func]), has:

  (3.1) unsafe-qualified pointer-to-function type, if that function is one
  to which an *attribute-specifier-seq* containing the attribute-token
  `unsafe` appertains
  ([§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) (1.2)), or a
  function declared with C language linkage and no *function-body*
  ([dcl.link], [dcl.fct.def.general]);

  (3.2) pointer-to-function type that is not unsafe-qualified, otherwise.

[Note: (3.1)'s second case is an `extern "C"` declaration with no body;
calling it is already a gated operation
([§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) (5.6)) for the same
reason: taking its address must not produce a pointer-to-function type a
caller could invoke without ever entering an unsafe context. — end note]

(4) A prvalue of pointer-to-function type that is not unsafe-qualified can
be converted to a prvalue of the otherwise-identical unsafe-qualified
pointer-to-function type. There is no implicit conversion in the other
direction.

[Note: this parallels [conv.fctptr]'s rule that a pointer to a `noexcept`
function converts to a pointer to an otherwise-identical non-`noexcept`
function, and not the reverse: conversion is permitted only towards the
type that promises less to the code holding the resulting pointer, never
towards the type that promises more than what produced it. — end note]

(5) A function call ([expr.call]) whose *postfix-expression* is a prvalue
of unsafe-qualified pointer-to-function type is a gated operation
([§3.4](00-front-matter.md#3-terms-and-definitions)).

[Note: [§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) (5.7) already
gates a function call whose *postfix-expression* denotes a function, by
name, to which the attribute-token `unsafe` appertains; that paragraph does
not by itself extend to a call through a pointer obtained from such a
function, whose *postfix-expression* denotes a pointer value rather than
the function itself. This paragraph closes that gap. — end note]

```cpp
[[scpp::unsafe]] int get_unchecked(int* base, int index) { return base[index]; }
int add(int a, int b) { return a + b; }

int (* [[scpp::unsafe]] up)(int*, int) = get_unchecked;   // OK: (3.1)
int (*                  sp)(int, int)  = add;             // OK: (3.2)

int (* [[scpp::unsafe]] up2)(int, int) = add;   // OK: (4), a widening
                                                  // conversion
int (*                  sp2)(int*, int) = get_unchecked;  // ill-formed: (4)
                                    // permits no conversion in this direction

int r1 = up(base, 0);                       // ill-formed: (5), a safe context
int r2;
[[scpp::unsafe]] { r2 = up(base, 0); }      // OK: an unsafe context
int r3 = sp(1, 2);                          // OK: sp is not unsafe-qualified
```

---

[← Previous: Front Matter](00-front-matter.md) · [Table of Contents](README.md) · [Next: Ownership, Initialization, and Move →](02-ownership-and-move.md)
