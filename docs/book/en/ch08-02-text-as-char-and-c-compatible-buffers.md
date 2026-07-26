# Text as `char` and C-Compatible Buffers

Section 8.1 established the storage story for fixed-size arrays in general. This
section narrows that idea to `char`: string literals, raw C strings, and
writable buffers that keep their bytes inline.

That low-level representation still matters in current scpp. `std::string`
exists and is useful when you want owned growable text, but a lot of APIs --
especially `extern "C"` boundaries -- still speak in `const char*` and
`char[N]`.

For each runnable example below, save the file as `char-buffers.scpp`, then
build and run it like this:

```sh
scpp char-buffers.scpp -o char-buffers
./char-buffers
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## String literals are read-only `const char*`

A string literal gives you borrowed read-only text.

```cpp
import std;

int main() {
    const char* greeting = "hello";
    std::println("{} {}", greeting, greeting[1]);
    return 0;
}
```

Output:

```text
hello e
```

`greeting` does not own a buffer that `main` allocated itself. It is just a
pointer to read-only bytes that already exist for the program, and indexing that
pointer reads individual characters.

## A `char[N]` array owns a writable text buffer inline

When you need bytes that your own function can edit, a fixed-size `char` array
works like any other array from Section 8.1. The one extra rule is that C-style
text must end with `\0` so another API knows where the text stops.

```cpp
extern "C" int puts(const char* s);

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '!';
    word[5] = '\0';

    [[scpp::unsafe]] {
        puts(word);
    }
    return 0;
}
```

Output:

```text
scpp!
```

`word` owns six `char` elements directly. The call to `puts` is inside
`[[scpp::unsafe]]` for the same reason as every other `extern "C"` call in this
book: the compiler cannot inspect the real implementation on the other side.

The final `\0` matters just as much as the visible letters. `puts` keeps reading
until it sees that terminator.

## Passing a `char[N]` buffer gives pointer-style access to the same storage

At a call boundary, an array buffer can be passed to a parameter written with
array syntax, and the callee updates the caller's storage in place.

```cpp
import std;

void make_excited(char text[6]) {
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

    make_excited(word);
    const char* text = word;
    std::println("{}", text);
    return 0;
}
```

Output:

```text
scpp!
```

`make_excited` did not receive a fresh copied array. It wrote through access to
the same underlying buffer, so `word[4]` changed in the caller too.

## `std::string` owns growable text and `c_str()` bridges to C APIs

A fixed-size `char[N]` buffer is good when the size is known up front. When you
want owned text that can grow, use `std::string` and borrow a C-compatible view
only at the boundary that needs it.

```cpp
import std;

extern "C" int puts(const char* s);

int main() {
    std::string name{"scpp"};
    name.append(" book");

    [[scpp::unsafe]] {
        puts(name.c_str());
    }

    int letters = static_cast<int>(name.size());
    std::println("{}", letters);
    return 0;
}
```

Output:

```text
scpp book
9
```

This keeps ownership with `name`, not with the C API. `c_str()` gives the
borrowed nul-terminated pointer that `puts` needs, while `std::string` stays the
better tool for dynamically sized text inside ordinary scpp code.

## A string literal cannot initialize a mutable `char*`

A literal is read-only text, so scpp does not let you treat it as a writable
buffer.

```cpp
int main() {
    char* text = "hi";
    return 0;
}
```

Compiler output:

```text
string_literal_mutable_pointer_fail.scpp:2:18: error: cannot initialize or assign raw pointer 'text' from an incompatible pointer type without an explicit cast
```

If code intends to write through the pointer, it needs actual writable storage --
for example, a `char[N]` array it owns itself. If code only needs to read the
literal, use `const char*` instead.

## The working rules for text buffers today

- string literals are borrowed read-only text, normally spelled `const char*`;
- a writable C-style text buffer is usually a fixed-size `char[N]` array with an
  explicit trailing `\0`;
- passing that array to another function gives pointer-style access to the same
  underlying bytes;
- `std::string` is the better owning type when text needs to grow, and
  `c_str()` hands a borrowed C-compatible pointer to another API;
- a string literal cannot initialize a mutable `char*` directly.

The next section returns to borrowed views, using `std::span` to talk about a
contiguous buffer without transferring ownership of it.

---

[← Previous: Fixed-Size Arrays](ch08-01-fixed-size-arrays.md) · [Table of Contents](README.md) · [Next: Borrowed Views with `std::span` →](ch08-03-borrowed-views-with-std-span.md)
