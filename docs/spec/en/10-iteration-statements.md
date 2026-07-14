# 8 Iteration statements

## 8.1 General [stmt.iter]

(1) Except as modified by this clause, [stmt.iter], [stmt.while],
[stmt.for], and [stmt.ranged] apply unchanged to iteration statements in
an SCPP26 program.

(2) An iteration statement in SCPP26 is one of:

  (2.1) a `while` statement;

  (2.2) a classic `for` statement of the form
  `for ( init-statementopt ; conditionopt ; expressionopt ) statement`;
  or

  (2.3) a range-based `for` statement of the form
  `for ( for-range-declaration : for-range-initializer ) statement`.

## 8.2 Classic `for` statements [stmt.for]

(1) A classic `for` statement executes with the same sequencing and
control-flow as in C++26:

  (1.1) its `init-statement`, if present, is executed exactly once before
  the first evaluation of the condition;

  (1.2) its condition, if present, is evaluated before each iteration,
  and if it evaluates to `false` the loop is exited without executing
  the loop body for that iteration; and

  (1.3) its iteration expression, if present, is evaluated after each
  completed iteration of the loop body, including when that iteration
  ends by `continue`.

(2) If the condition is omitted, it is treated as if it were `true`.

(3) If the `init-statement` is a declaration, it is a local-variable
declaration governed by
[§6.1](02-ownership-and-move.md#61-required-initialization-and-zero-initialization-dclinit),
including that subclause's requirement that the declared local variable
have an explicit initializer.

(4) If the `init-statement` is a declaration, the names it introduces are
in scope for the condition, the iteration expression, and the loop body,
and leave scope when the `for` statement is exited.

## 8.3 Range-based `for` statements [stmt.ranged]

(1) The `for-range-declaration` is a local-variable declaration,
appearing without its trailing `;`, and is governed by
[§6.1](02-ownership-and-move.md#61-required-initialization-and-zero-initialization-dclinit)
except as modified by this subclause. It declares exactly one loop
variable.

(2) The `for-range-initializer` is evaluated exactly once.

(3) In SCPP26 v1, the `for-range-initializer` shall have one of the
following types:

  (3.1) a fixed-size array type;

  (3.2) `std::span<T>`; or

  (3.3) `std::span<const T>`.

(4) A range-based `for` statement iterates over the elements of its
`for-range-initializer` in increasing subscript order, beginning at
subscript `0` and ending after the last valid subscript.

(5) Before each iteration, the loop variable is initialized from the
current element exactly as an ordinary declaration of that loop
variable's type would be initialized from that element.

(6) If the loop variable is declared by value, it receives a fresh
element copy each iteration; an assignment to that loop variable does
not modify the underlying array or span element.

(7) If the loop variable is declared by reference, it refers to the
underlying element selected for the current iteration, exactly as an
ordinary reference declaration would.

(8) If the loop variable is declared by reference, and its initializer is
obtained through a live mutable reference or mutable `std::span<T>`
local variable or parameter, that binding is a reborrow governed by
[§6.2](02-ownership-and-move.md#62-ownership-move-state-and-reborrows-basiclife)
(7)-(10).

```cpp
int values[3]{1, 2, 3};

for (int i = 2; i >= 0; i = i - 1) {
    values[i] = values[i] + 1;
}

for (auto& value : values) {
    value = value + 10;   // modifies the underlying array element
}

for (const auto& value : values) {
    // value = 0;         // ill-formed
}

std::span<int> view{values};
for (auto& value : view) {
    value = value + 1;    // value is a reborrow of the current span element
}
```

---

[← Previous: Enumeration conversions](09-enumeration-conversions.md) · [Table of Contents](README.md) · [Next: Inheritance and Interfaces →](11-inheritance-and-interfaces.md)
