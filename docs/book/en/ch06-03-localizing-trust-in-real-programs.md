# Localizing Trust in Real Programs

The last two sections explained what `[[scpp::unsafe]]` gates and how raw
pointers or `extern "C"` calls cross that boundary.

This section answers the next practical question: **where should unsafe code
live in a real program?**

The general rule is simple:

- keep unsafe regions as small as possible;
- prefer a plain safe function with a tiny internal unsafe block when the
  function can fully vouch for itself;
- use a function-level `[[scpp::unsafe]]` only when the caller must uphold a
  precondition that the function body cannot prove on its own.

For each runnable example below, save the file as `trust.scpp`, then build and
run it like this:

```sh
scpp trust.scpp -o trust
./trust
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Prefer a small unsafe block inside an ordinary safe function

If a function can fully control and justify the unsafe operation by itself,
keep the function ordinary and make only the critical operation unsafe.

```cpp
import std;

int first_of_pair(int left, int right) {
    int values[2]{};
    values[0] = left;
    values[1] = right;
    int* pointer = &values[0];
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    std::println("{}", first_of_pair(11, 22));
    return 0;
}
```

Output:

```text
11
```

The caller does not need to know anything about raw pointers here. The function
itself created the local array, formed the pointer, and dereferenced it in one
tiny place it could justify directly.

## Use function-level `[[scpp::unsafe]]` when the caller must vouch

Sometimes the function body cannot make the operation sound on its own. If the
function accepts a raw pointer from outside, its correctness depends on a
precondition only the caller can guarantee.

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

Here the unsafety is part of the function's contract, not just an internal
implementation detail.

## That contract propagates to the call site

Because the caller is the one who must vouch for the input, calling such a
function from safe code is rejected.

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
call_unsafe_wrapper_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

So a function-level `[[scpp::unsafe]]` should be a conscious API decision. It
makes the caller share responsibility for the safety argument.

## Keep the unsafe boundary narrow even in larger wrappers

Real programs often need to call several foreign functions, but the same rule
still applies: keep each unsafe region close to the exact call that needs it,
and keep the rest of the logic ordinary safe code.

```cpp
import std;

extern "C" {
    int socket(int domain, int type, int protocol);
    int getsockopt(int fd, int level, int optname, void* optval, int* optlen);
    int close(int fd);
}

int query_socket_type() {
    int fd = 0;
    [[scpp::unsafe]] {
        fd = socket(2, 2, 0);
    }

    int value = 0;
    int len = 4;
    [[scpp::unsafe]] {
        getsockopt(fd, 1, 3, &value, &len);
        close(fd);
    }
    return value;
}

int main() {
    std::println("{}", query_socket_type());
    return 0;
}
```

Output:

```text
2
```

Most of `query_socket_type` is still normal code: local variables, return
values, and control flow. Only the foreign calls themselves are fenced off.

## Even a large unsafe region still keeps the checker on

Making a whole block unsafe does **not** disable ownership checking. It is still
better to keep unsafe code small, but even when you do need a broad unsafe
region, scpp continues checking moves and borrows.

```cpp
import std;

int f() {
    [[scpp::unsafe]] {
        std::unique_ptr<int> first = std::make_unique<int>(1);
        std::unique_ptr<int> second = std::move(first);
        std::unique_ptr<int> third = std::move(first);
        return *third;
    }
}

int main() {
    return f();
}
```

Compiler output:

```text
unsafe_whole_body_still_checks_moves_fail.scpp:7:38: error: use of moved-out variable 'first'
 7 |         std::unique_ptr<int> third = std::move(first);
   |                                      ^
```

That is the real meaning of “localizing trust”: the unsafe part should be only
the part the compiler truly cannot justify. Let the rest of the function stay
in the ordinary checked world.

The next chapter moves away from these low-level boundaries and into project
structure: packages, modules, and manifests.

---

[← Previous: Calling `extern "C"` and Using Raw Pointers](ch06-02-calling-extern-c-and-using-raw-pointers.md) · [Table of Contents](README.md)
