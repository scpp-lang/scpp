# Control Flow

Variables hold values, and functions package work into reusable pieces. Control
flow decides which piece of work runs next, and how many times it runs.

In current scpp, the main control-flow tools available to learners are `if`,
`while`, classic `for`, and range-based `for`. This section introduces each of
them with small programs you can build and run today.

For each short example below, save the file as `concepts.scpp`, then build and
run it like this:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## `if` runs code only when a condition is true

Use `if` when a block of code should run only if a condition holds.

```cpp
import std;

int main() {
    int temperature = 33;

    if (temperature > 30) {
        std::println("It is warm outside.");
    }

    return 0;
}
```

Output:

```text
It is warm outside.
```

The condition inside `if (...)` should already be a `bool`. Here, `temperature >
30` is a comparison, so it produces exactly the kind of value that `if` expects.

## `else if` and `else` choose between paths

When a program has more than one possible path, chain conditions together with
`else if`, and finish with `else` for the fallback case.

```cpp
import std;

int main() {
    int score = 85;

    if (score < 60) {
        std::println("try again");
    } else if (score < 90) {
        std::println("you passed");
    } else {
        std::println("excellent");
    }

    return 0;
}
```

Output:

```text
you passed
```

The branches are checked from top to bottom. As soon as one condition is true,
that branch runs and the later ones are skipped.

## `while` repeats work while a condition stays true

Use `while` when the same block of code should keep running until some condition
stops being true.

```cpp
import std;

int main() {
    int count = 3;

    while (count > 0) {
        std::println("{}!", count);
        count = count - 1;
    }

    std::println("Go!");
    return 0;
}
```

Output:

```text
3!
2!
1!
Go!
```

A `while` loop needs a condition and some state that changes. If `count` never
changed, the loop would never end.

## Classic `for` keeps the loop setup in one place

A classic `for` loop combines three parts in one header:

- an initial step that runs once;
- a condition checked before each iteration;
- an update step that runs after each completed iteration.

```cpp
import std;

int main() {
    int total = 0;

    for (int i = 1; i <= 5; i = i + 1) {
        total = total + i;
    }

    std::println("total = {}", total);
    return 0;
}
```

Output:

```text
total = 15
```

This is the same counting pattern you could write with `while`, but `for` keeps
the loop variable, the stop condition, and the per-turn update together.

## Range-based `for` walks over each element in order

When you want to visit every element of an array, a range-based `for` loop is
often the clearest choice.

```cpp
import std;

int main() {
    int scores[3]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    int total = 0;

    for (int score : scores) {
        total = total + score;
    }

    std::println("total = {}", total);
    return 0;
}
```

Output:

```text
total = 60
```

Here the loop variable `score` is initialized from each array element in turn.
Because it is declared by value, changing `score` itself would not change the
underlying array.

## Range-based `for` also works with `std::span`

Range-based `for` is not limited to fixed-size arrays. It also works with
`std::span`, which is scpp's borrowed view type for a sequence of elements.

```cpp
import std;

int main() {
    int values[3]{};
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    std::span<int> view = values;

    for (auto& value : view) {
        value = value * 2;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

Output:

```text
2
4
6
```

Because the loop variable is `auto&`, each `value` refers to the underlying
element inside the span. Updating `value` therefore updates the original array
too.

## A practical rule

When you are deciding which control-flow tool to use, keep these rules in mind:

- use `if` when you want to choose between paths;
- use `while` when you want to repeat work;
- use classic `for` when you have a loop variable, a stop condition, and an
  update step that belong together;
- use range-based `for` when you want to visit each element of an array or
  `std::span` in order;
- make your conditions explicit comparisons that already produce `bool` values;
- when a loop should eventually stop, update the state that the condition reads.

That is enough to understand every example you have seen so far in the guessing
game chapter. It is also enough to write many small command-line programs that
choose between paths, repeat work, count through a range, or walk across a
sequence element by element.

---

[← Previous: Comments](ch03-04-comments.md) · [Table of Contents](README.md) · [Next: What Is Ownership? →](ch04-01-what-is-ownership.md)
