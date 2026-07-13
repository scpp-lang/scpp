# Functions

Variables give names to values. Functions give names to reusable work.

This is the next natural step after data types: once you know what kinds of
values you have, you can start packaging calculations into pieces you can call
again.

For each short example below, save the file as `concepts.scpp`, then build and
run it like this:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## Defining and calling a function

A function definition has four familiar parts:

- the return type,
- the function name,
- the parameter list in parentheses,
- and the body in braces.

```cpp
import std;

int double_value(int value) {
    return value * 2;
}

int main() {
    int doubled = double_value(21);

    std::println("doubled = {}", doubled);
    return 0;
}
```

Output:

```text
doubled = 42
```

`double_value(21)` calls the function, gives `21` to its parameter, and gets one
`int` back.

## Parameters are just local names

Inside a function, each parameter behaves like a local variable with an initial
value.

```cpp
import std;

int add_one(int value) {
    value = value + 1;
    return value;
}

int main() {
    int score = 10;
    int next = add_one(score);

    std::println("score = {}, next = {}", score, next);
    return 0;
}
```

Output:

```text
score = 10, next = 11
```

Changing `value` inside `add_one` does not rewrite `score` in `main`. The
function works with its own parameter variable and then returns the result.

## Functions can return `bool`

Functions do not have to return numbers. A function can also return a `bool`
that another part of the program can use directly.

```cpp
import std;

bool can_level_up(int score, int bonus) {
    return score + bonus >= 100;
}

int main() {
    bool ready = can_level_up(80, 25);

    std::println("ready = {}", ready);
    return 0;
}
```

Output:

```text
ready = true
```

This style is useful when a function answers a question instead of computing a
new number or piece of text.

## One important type rule

Function parameter types and return types are explicit, and scpp keeps scalar
types separate. That means calls should use the types a function actually asks
for, instead of depending on hidden implicit conversions.

In practice, that makes function calls easier to read: the function signature
tells you exactly what kind of values belong there.

The next section will stay small and practical again, but look at comments: how
to explain code to human readers without changing what the compiler sees.

---

[← Previous: Scalar Data Types](ch03-02-scalar-data-types.md) · [Table of Contents](README.md) · [Next: Comments →](ch03-04-comments.md)
