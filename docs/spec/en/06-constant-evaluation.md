# 7 Constant evaluation

## 7.1 General [expr.const]

(1) Except as modified by this clause, [expr.const] applies unchanged to a
SCPP26 program.

(2) An expression that this document or the C++ standard requires to be a
constant expression is said in this clause to undergo **required constant
evaluation**.

(3) Required constant evaluation is well-formed only if every operation
whose evaluation is necessary to determine the expression's value is one
permitted by 7.2 and no operation listed in 7.3 is evaluated.

(4) A conforming implementation shall perform required constant
evaluation using the target program's semantic model, including the
target's scalar-value ranges, floating-point semantics, and pointer
model.

[Note: this clause constrains only evaluation that is required to produce
a constant expression. A call to a `constexpr` function outside such a
context remains an ordinary runtime computation unless the C++ standard
or another clause of this document requires otherwise. — end note]

## 7.2 Supported subset [expr.const.scpp.support]

(1) For the purposes of this clause, a type is **constexpr-compatible**
if it is:

  (1.1) `bool`, a character type, a signed or unsigned integer type,
  `float`, `double`, or another standard floating-point type provided by
  the implementation;

  (1.2) a pointer type, but only for the values permitted by (3);

  (1.3) an array type whose element type is constexpr-compatible;

  (1.4) a trivial struct type every non-static data member of which is
  constexpr-compatible; or

  (1.5) a class type satisfying (2).

(2) A class type satisfies this paragraph if:

  (2.1) every non-static data member of the class type is
  constexpr-compatible;

  (2.2) no non-static data member of the class type is declared
  `mutable`;

  (2.3) every constructor whose evaluation is required is declared
  `constexpr` or `consteval`; and

  (2.4) no evaluation path requires execution of a user-defined
  destructor.

(3) During required constant evaluation, a pointer value is permitted
only if it is:

  (3.1) a null pointer value;

  (3.2) a pointer designating an element or the one-past-the-end element
  of a string-literal object; or

  (3.3) a pointer designating a subobject, element, or one-past-the-end
  element of an object with static storage duration that is
  constant-initialized and not modified during the evaluation.

(4) During required constant evaluation, the implementation shall
support the formation and use of:

  (4.1) string-literal objects;

  (4.2) fixed-size arrays of constexpr-compatible element type; and

  (4.3) objects of type `std::span<const T>`, where `T` is
  constexpr-compatible and the span's range lies within an object
  described by (4.1) or (4.2).

(5) During required constant evaluation, the implementation shall
support evaluation of:

  (5.1) block statements;

  (5.2) local declarations;

  (5.3) assignments;

  (5.4) `if` statements;

  (5.5) `while` statements;

  (5.6) classic `for` statements and range-based `for` statements;

  (5.7) `return` statements;

  (5.8) recursive calls; and

  (5.9) pack-expanded expressions and statements after template
  substitution.

(6) During required constant evaluation, the implementation shall
support:

  (6.1) calls to `constexpr` functions and constructors;

  (6.2) calls to `consteval` functions and constructors; and

  (6.3) arithmetic and comparison operations on integer, character, and
  floating-point operands; and

  (6.4) `sizeof(type-id)`, `sizeof(expression)`, and `alignof(type-id)`
  queries whose operands are otherwise well-formed.

## 7.3 Unsupported operations [expr.const.scpp.unsupported]

(1) If required constant evaluation would evaluate a `[[scpp::unsafe]]`
compound-statement or the body of a function to which
`[[scpp::unsafe]]` appertains, the program is ill-formed.

(2) If required constant evaluation would evaluate:

  (2.1) a call through `extern "C"` or another foreign-function
  interface;

  (2.2) a call whose definition is unavailable for constant evaluation,
  including an imported definition whose compile-time body is not
  provided; or

  (2.3) a call whose selected operation requires runtime-only dynamic
  dispatch,

the program is ill-formed.

(3) If required constant evaluation would evaluate `new`, `delete`, any
other dynamic storage allocation or deallocation, or any operation whose
correctness depends on dynamic lifetime management, the program is
ill-formed.

(4) If required constant evaluation would read from or write to a union
member, mutate an object through a raw pointer, evaluate a
lambda-expression, throw an exception, perform input or output, create or
synchronize a thread, access the environment or the filesystem, or
execute any operation that requires a user-defined destructor, the
program is ill-formed.

[Note: this clause specifies the SCPP26 v1 constant-evaluation subset
explicitly. A program outside that subset is not yet accepted merely
because C++26 would otherwise permit the corresponding constant
evaluation. — end note]

## 7.4 Failure of required constant evaluation [expr.const.scpp.fail]

(1) If required constant evaluation would call a function that is neither
declared `constexpr` nor declared `consteval`, and that call is not
otherwise permitted by [expr.const], the program is ill-formed.

(2) If required constant evaluation fails to produce a result because it
would evaluate:

  (2.1) arithmetic overflow in a checked arithmetic operation;

  (2.2) a division operator or remainder operator with a zero right
  operand;

  (2.3) an out-of-bounds access to an array, string-literal object, or
  `std::span`; or

  (2.4) an access to a value that is not available to constant
  evaluation,

the program is ill-formed.

(3) A conforming implementation shall impose finite limits on:

  (3.1) nested constant-evaluation depth;

  (3.2) total constant-evaluation steps; and

  (3.3) the number of iterations of a single loop evaluated during
  required constant evaluation.

(4) The limits in (3) shall be no less than 512 nested evaluations,
1,000,000 total evaluation steps, and 262,144 iterations of a single
loop.

---

[← Previous: Union types and packed layout](05-unions-and-packed-layout.md) · [Table of Contents](README.md) · [Next: The `constexpr` and `consteval` specifiers →](07-constexpr-and-consteval.md)
