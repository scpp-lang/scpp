# Scalar Data Types

The last section focused on how variables are declared. This one shifts to the
values that those variables hold.

For early scpp programs, four scalar types cover a lot of ground:

- `int` for whole numbers,
- `double` for decimal values,
- `bool` for true-or-false decisions,
- `char` for one character at a time.

As before, save each short example as `concepts.scpp`, then build and run it:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## Whole numbers and decimal numbers

`int` is the usual starting point for counting, indexing, and other
whole-number work. `double` is the common choice when fractions matter.

```cpp
import std;

int main() {
    int left = 10 - 3;
    double price = 1.25 + 0.5;

    std::println("left = {}, price = {}", left, price);
    return 0;
}
```

Output:

```text
left = 7, price = 1.75
```

Keep those calculations in their own types. The `int` expression stays entirely
integer-based, and the `double` expression stays entirely floating-point.

If you later need an exact width instead of the friendly `int` / `double`
spellings, scpp also provides names such as `int32_t`, `uint64_t`, and
`float64_t`.

## `bool` is for real conditions

Conditions in scpp should already be `bool`. A comparison gives you that `bool`
directly.

```cpp
import std;

int main() {
    int lives = 3;
    bool keep_playing = lives > 0;

    if (keep_playing) {
        std::println("keep playing");
        return 0;
    }

    std::println("game over");
    return 0;
}
```

Output:

```text
keep playing
```

That is a small but important difference from ordinary C++. scpp does not ask
you to treat arbitrary integers as “truthy” or “falsey”. Instead, write a real
comparison such as `lives > 0` and keep the result in a `bool`.

## `char` holds one character

Use `char` when one byte-sized character value is enough.

```cpp
import std;

int main() {
    char grade = 'A';

    std::println("grade = {}", grade);
    return 0;
}
```

Output:

```text
grade = A
```

Single quotes matter here. `'A'` is one `char`; `"A"` would be text, not a
single character value.

## One more rule to remember

scpp keeps scalar types separate. `bool`, `char`, `int`, and `double` do not
silently convert into one another. That is why the examples above keep each
expression within one scalar type and use comparisons when a `bool` is needed.

This may feel stricter at first, but it makes the type of every calculation
more obvious at a glance.

The next section will keep building on these basics by packaging calculations
and actions into named functions.

---

[← Previous: Variables and Explicit Initialization](ch03-01-variables-and-explicit-initialization.md) · [Table of Contents](README.md) · [Next: Functions →](ch03-03-functions.md)
