# Borrowed Views with `std::span`

Section 4.3 introduced `std::span` from the ownership side: a span is a borrow
of contiguous elements, not a new owner. Now that this chapter has arrays and
character buffers on the table, we can return to spans as the normal function
boundary for “some existing buffer of `T` values”.

A fixed-size array answers “where are the elements stored?” A span answers “how
can another function use those same elements without copying or taking
ownership?”

For each runnable example below, save the file as `span-buffers.scpp`, then
build and run it like this:

```sh
scpp span-buffers.scpp -o span-buffers
./span-buffers
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## One `std::span<const T>` parameter can read arrays with different bounds

A borrowed view lets one function accept any fixed-size array of the right
element type, even when the lengths differ.

```cpp
import std;

int sum(std::span<const int> values) {
    int total = 0;
    for (int value : values) {
        total = total + value;
    }
    return total;
}

int main() {
    int first[3]{};
    first[0] = 10;
    first[1] = 20;
    first[2] = 30;

    int second[5]{};
    second[0] = 1;
    second[1] = 2;
    second[2] = 3;
    second[3] = 4;
    second[4] = 5;

    std::println("{} {}", sum(first), sum(second));
    return 0;
}
```

Output:

```text
60 15
```

Neither call copies the array elements. Each call constructs a small read-only
view at the boundary, and `sum` reads through that view.

## A mutable span can rewrite a borrowed `char` buffer in place

The previous section built C-compatible text buffers with `char[N]`. A helper
that should edit that existing buffer can take `std::span<char>`.

```cpp
import std;

void shout(std::span<char> text) {
    text[0] = 'S';
    text[4] = '!';
    return;
}

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '?';
    word[5] = '\0';

    shout(word);
    const char* view = word;
    std::println("{}", view);
    return 0;
}
```

Output:

```text
Scpp!
```

`shout` did not receive ownership of the buffer. It only borrowed the six
existing characters and wrote through that borrow.

## A span carries the current length of the borrowed buffer

Unlike a raw pointer by itself, a span keeps the element count next to the data.
That makes it a better fit for loops that should stay inside the borrowed
buffer's bounds.

```cpp
import std;

int count_visible(std::span<const char> text) {
    int used = 0;
    while (used < text.size && text[used] != '\0') {
        used = used + 1;
    }
    return used;
}

int main() {
    char title[8]{};
    title[0] = 'b';
    title[1] = 'o';
    title[2] = 'o';
    title[3] = 'k';
    title[4] = '\0';

    std::println("{}", count_visible(title));
    return 0;
}
```

Output:

```text
4
```

Here the visible text ends at the first `\0`, but the borrowed buffer itself is
still eight elements wide. `std::span<const char>` lets the function respect
both facts at once: it can stop at the text terminator without ever reading past
the actual array.

## A span cannot outlive the storage it borrows

Because a span is only a view, returning one that points into a local array is
rejected.

```cpp
std::span<int> bad() {
    int local[2]{};
    return local;
}

int main() {
    return 0;
}
```

Compiler output:

```text
span_return_local_fail.scpp:3:5: error: function 'bad' returns a lifetime-tracked value from an incompatible source type
```

That is the same ownership story as before, applied to buffer views: once the
local array dies, any span that pointed into it would be dangling too, so scpp
rejects the program.

## The working rules for spans in buffer-oriented code

- keep arrays and `char[N]` buffers as the owners of their own storage;
- use `std::span<const T>` when a function should read an existing contiguous
  buffer without copying it;
- use `std::span<T>` when a function should mutate that borrowed buffer in
  place;
- a span carries both the starting address and the current element count;
- a span still follows ownership and lifetime rules, so it cannot outlive the
  storage it borrows.

With this chapter's arrays, text buffers, and spans in place, we now have the
basic vocabulary for contiguous data in today's scpp: owned inline storage,
C-compatible character buffers, and borrowed views over both.

---

[← Previous: Text as `char` and C-Compatible Buffers](ch08-02-text-as-char-and-c-compatible-buffers.md) · [Table of Contents](README.md)
