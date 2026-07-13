# Comments

Comments are for human readers. They let you explain intent, mark a tricky
detail, or leave a short note near a piece of code without changing what the
compiler does.

That makes comments a small topic, but an important one: good comments can make
the difference between code that merely works and code that is easy to revisit
later.

For each short example below, save the file as `concepts.scpp`, then build and
run it like this:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## Line comments

The most common form is a line comment starting with `//`.

```cpp
import std;

int main() {
    // This comment is for people, not for the compiler.
    int answer = 40 + 2;

    std::println("answer = {}", answer);
    return 0;
}
```

Output:

```text
answer = 42
```

Everything after `//` on that line is ignored by the compiler.

## Block comments

When one short line is not enough, use a block comment.

```cpp
import std;

int main() {
    /* A block comment can cover more than one line
       when one short note is not enough. */
    int total = 20 + 22;

    std::println("total = {}", total);
    return 0;
}
```

Output:

```text
total = 42
```

Block comments are useful for a slightly longer explanation that still belongs
right next to the code it describes.

## Short comments beside a statement

Sometimes the clearest note is a tiny reminder at the end of the line it
explains.

```cpp
import std;

int main() {
    int score = 7; // starting score for this round
    score = score + 5;

    std::println("score = {}", score);
    return 0;
}
```

Output:

```text
score = 12
```

This style works best when the comment is brief. If the explanation gets long,
move it above the statement instead.

## A practical rule

Good comments explain *why*, not just *what*. If code already says exactly what
it is doing, repeating that in prose usually adds noise. Save comments for:

- intent that is not obvious from the code alone;
- assumptions a reader should know;
- quick orientation in a longer function or module.

The next section returns to program behavior and control flow: choosing between
paths with `if` and repeating work with `while`.

---

[← Previous: Functions](ch03-03-functions.md) · [Table of Contents](README.md) · [Next: Control Flow →](ch03-05-control-flow.md)
