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

---

[← Previous: The `constexpr` and `consteval` specifiers](07-constexpr-and-consteval.md) · [Table of Contents](README.md) · [Next: Enumeration conversions →](09-enumeration-conversions.md)
