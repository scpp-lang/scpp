# What `[[scpp::unsafe]]` Does and Does Not Do

scpp is designed so most code stays in the checked, ordinary part of the
language. Sometimes, though, a program really does need to do something the
compiler cannot prove safe on its own, such as dereferencing a raw pointer or
calling an unchecked foreign function.

That is what `[[scpp::unsafe]]` is for.

Just as important, `[[scpp::unsafe]]` is deliberately narrow. It does **not**
turn off ownership checking, borrow checking, or lifetime checking. It only
opens specific safety boundaries that the programmer must justify locally.

For each runnable example below, save the file as `unsafe.scpp`, then build and
run it like this:

```sh
scpp unsafe.scpp -o unsafe
./unsafe
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Using an unsafe block

The most common form is an unsafe block. Code outside the block is still normal
safe scpp code.

```cpp
import std;

int read_value(int* pointer) {
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    int value{42};
    std::println("{}", read_value(&value));
    return 0;
}
```

Output:

```text
42
```

Forming the raw pointer with `&value` is fine in safe code. The unsafe boundary
appears exactly where the program chooses to trust that pointer and dereference
it.

## The same operation is rejected outside `[[scpp::unsafe]]`

If you try to dereference the raw pointer in ordinary safe code, the compiler
stops you.

```cpp
int read_value(int* pointer) {
    return *pointer;
}

int main() {
    int value{42};
    return read_value(&value);
}
```

Compiler output:

```text
raw_pointer_unsafe_fail.scpp:2:12: error: cannot dereference raw pointer 'pointer': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)
 2 |     return *pointer;
   |            ^
```

So `[[scpp::unsafe]]` is not a style hint. It is the gate that makes certain
operations well-formed at all.

## You can also mark a whole function as unsafe

If an entire helper function exists to perform an unsafe operation, you can put
the attribute on the function itself.

```cpp
import std;

[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    [[scpp::unsafe]] {
        std::println("{}", read_first(&value));
    }
    return 0;
}
```

Output:

```text
9
```

This means the whole body of `read_first` is an unsafe context. It does **not**
mean callers are automatically safe.

## Calling an unsafe function also requires unsafe context

An unsafe-marked function carries an unchecked precondition, so the call site
must acknowledge that too.

```cpp
[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    return read_first(&value);
}
```

Compiler output:

```text
call_unsafe_function_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

That is the core design idea: unsafe assumptions should stay visible at the
exact place where code relies on them.

## `[[scpp::unsafe]]` does not disable borrow checking

Unsafe code is still checked for ownership and aliasing rules.

```cpp
int main() {
    int value{5};
    [[scpp::unsafe]] {
        int& first = value;
        int& second = value;
        return first + second;
    }
}
```

Compiler output:

```text
unsafe_still_checks_borrows_fail.scpp:5:9: error: cannot mutably borrow 'value': it is already borrowed
 5 |         int& second = value;
   |         ^
```

So `[[scpp::unsafe]]` does **not** mean “turn the checker off.” It only means
“permit one of the language's explicitly gated operations here.”

## What `[[scpp::unsafe]]` is for

At a high level, `[[scpp::unsafe]]` is the place where scpp lets you cross a
few specific boundaries, such as:

- dereferencing raw pointers or doing pointer arithmetic;
- calling bodyless `extern "C"` functions;
- accessing union members;
- using raw `new` or `delete`;
- calling functions whose own declarations are marked `[[scpp::unsafe]]`.

The next section will look at one of the most common cases in more detail:
calling `extern "C"` functions and working with raw pointers.

---

[← Previous: Methods and `this`](ch05-03-methods-and-this.md) · [Table of Contents](README.md)
