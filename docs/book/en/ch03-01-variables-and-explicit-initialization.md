# Variables and Explicit Initialization

The guessing game used local variables to remember the secret number and the
player's latest guess. Now we can slow down and look at those declarations by
themselves.

For the short examples in this section, save each file as `concepts.scpp`, then
build and run it like this:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## Local variables are mutable by default

A plain local variable gives a name to a value, and that value may change later
in the same scope.

```cpp
import std;

int main() {
    int counter = 0;
    counter = counter + 1;
    counter = counter + 1;

    std::println("counter = {}", counter);
    return 0;
}
```

Output:

```text
counter = 2
```

The important thing is not just that `counter` changed. It also kept the same
type for its whole lifetime: once declared as `int`, it stays an `int`.

## `const` makes a local read-only

If a local should be assigned once and then stay fixed, write `const`.

```cpp
import std;

int main() {
    const int target = 21;
    int doubled = target + target;

    std::println("doubled = {}", doubled);
    return 0;
}
```

Output:

```text
doubled = 42
```

`target` is initialized at its declaration and then cannot be reassigned later.
That is useful when a name represents a value you want to protect from
accidental updates.

## Every local needs an initializer

Current scpp does not allow a bare local declaration such as `int score;`.
Every local must say how it starts at the point where it is declared.

```cpp
import std;

int main() {
    int score{};
    int level{3};
    int bonus = 7;
    bool finished{};

    std::println("score = {}, level = {}, bonus = {}, finished = {}", score, level, bonus, finished);
    return 0;
}
```

Output:

```text
score = 0, level = 3, bonus = 7, finished = false
```

This small example shows the three forms you will use most often:

- `int score{};` uses empty braces and requests the zero/default value;
- `int level{3};` uses braces with explicit constructor-style arguments;
- `int bonus = 7;` uses `=` with an expression on the right-hand side.

The important rule is that the initializer is not optional. scpp asks you to be
explicit up front, even when the value you want is just zero or `false`.

## A simple working habit

For everyday code, these three rules are enough to get started:

- use an ordinary local when the value should change;
- use `const` when the value should stay fixed after initialization;
- initialize every local immediately, instead of declaring it first and filling
  it in later;
- use `{}` when you want the zero/default value and `= value` or `{value}` when
  you already know the starting data.

The next section keeps the same tiny-program style, but looks more directly at
the data types that these variables hold.

---

[← Previous: Programming a Guessing Game](ch02-00-guessing-game.md) · [Table of Contents](README.md)
