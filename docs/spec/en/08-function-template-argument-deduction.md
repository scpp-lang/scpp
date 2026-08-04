# 13 Function template argument deduction

## 13.1 Deduction from a function call [temp.deduct.call.scpp]

(1) Except as modified by this subclause, [temp.deduct.call] applies
unchanged to deduction of template arguments from a function call in a
SCPP26 program.

(2) A function parameter pack in a function parameter list shall appear
only as the last parameter declaration in that list.

(3) Deduction from a function call is performed over the full function
parameter list. The compatibility of an earlier function parameter with
its corresponding argument is not finally determined before deduction
from every later non-defaulted function parameter has been considered.

(4) If the type of a function parameter *P* contains a template
parameter pack `Args...`, and that pack is first directly deduced from
one or more later function parameters, the implementation shall:

  (4.1) determine the final binding of `Args...` from those later
  deduction sources;

  (4.2) substitute that binding into *P*; and

  (4.3) then determine whether the corresponding argument satisfies *P*
  so substituted.

(5) If the substituted type in (4.2) is not satisfied by the
corresponding argument, the program is ill-formed.

(6) This subclause does not authorize a non-final function parameter
pack; (2) still applies when deduction proceeds as in (4).

## 13.2 Compound requirements in `requires`-expressions [expr.prim.req.compound.scpp]

(1) This subclause applies to a *compound-requirement*
([expr.prim.req.compound]) appearing within a *requires-expression*
([expr.prim.req]).

(2) Except as modified by this subclause, a *compound-requirement* in an
SCPP26 program is governed by the ordinary C++ rules.

(3) In an unqualified *compound-requirement* of the form `{ *E* }`,
where no *type-constraint* follows the closing `}` and no leading
`noexcept` appears, the expression *E* shall be one of:

  (3.1) a function call expression whose *postfix-expression* denotes a
  function or function object;

  (3.2) a member function call expression whose *postfix-expression*
  denotes a class member access selecting the called member function; or

  (3.3) a direct-initialization or list-initialization expression that
  constructs an object of a named type, written as `T(args...)` or
  `T{args...}`, where `T` names that type.

(4) If *E* is not one of the forms listed in (3), the program is
ill-formed.

(5) A *compound-requirement* whose expression is of the form in (3.3) is
satisfied if and only if, for the types determined by the probe
parameters and operands of the enclosing *requires-expression*, the
named type is constructible from the supplied argument types in the
written form.

(6) The satisfaction check in (5) is structural. It is performed as part
of determining whether the requirement written in the definition is
well-formed and satisfied for the probed types; it does not require any
distinguished compiler-internal concept name for constructibility.

[Note: this permits a concept to express copy-constructibility directly,
for example `requires(T t) { T{t}; }`, and more generally to express
constructibility from arbitrary argument types by forms such as
`requires(T t, U u) { T(t, u); }`. The meaning matches the ordinary C++20
use of construction expressions in compound requirements, such as those
used to define `copy_constructible` or `constructible_from`. — end note]

---

[← Previous: The `constexpr` and `consteval` specifiers](07-constexpr-and-consteval.md) · [Table of Contents](README.md) · [Next: Enumeration conversions →](09-enumeration-conversions.md)
