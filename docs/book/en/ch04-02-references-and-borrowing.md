# References and Borrowing

Ownership explains who is responsible for cleanup. References explain how code can *use* a value temporarily without taking that responsibility away.

In scpp, this uses real C++ reference syntax:

- `const T&` is a shared, read-only borrow;
- `T&` is a mutable borrow.

There is no Rust-style `&mut` spelling here. Instead, scpp's `movecheck` layer enforces Rust-inspired borrowing rules on top of ordinary C++ reference syntax.

For each runnable example below, save the file as `references.scpp`, then build and run it like this:

```sh
scpp references.scpp -o references
./references
```

For examples that are supposed to be rejected, save the file under the descriptive filename shown in the diagnostic block if you want the compiler output to match byte for byte.

## Borrowing with `const T&`

A function that only needs to look at a value can take a `const T&` instead of taking ownership.

```cpp
import std;

int calculate_length(const std::string& text) {
    return text.length();
}

int main() {
    std::string title{"scpp"};
    int length{calculate_length(title)};
    std::println("{} has {} bytes", title.c_str(), length);
    return 0;
}
```

Output:

```text
scpp has 4 bytes
```

`title` is still usable after the call because ownership never moved into `calculate_length`. The function only borrowed it.

We call this borrowing because the callee gets temporary access, but the caller stays the owner.

## Direct writes through `const T&` are rejected

A shared borrow is read-only.

```cpp
import std;

void change(const int& value) {
    value = 2;
    return;
}

int main() {
    int x{1};
    change(x);
    return 0;
}
```

The compiler rejects this:

```text
assign_through_const_ref_fail.scpp:4:5: error: cannot assign through 'value': it is a read-only (const) reference
 4 |     value = 2;
   |     ^
```

## Mutable borrows with `T&`

When a function should update the caller's value without becoming its owner, use `T&`.

```cpp
import std;

void add_suffix(std::string& text) {
    text.append(" book");
    return;
}

int main() {
    std::string title{"scpp"};
    add_suffix(title);
    std::println("{}", title.c_str());
    return 0;
}
```

Output:

```text
scpp book
```

`add_suffix` did not take ownership of `title`. It borrowed `title` mutably, changed it in place, and then returned. The caller remains the owner the whole time.

## Any number of shared borrows, or one mutable borrow

scpp enforces the same core shape you want for safe aliasing:

- any number of `const T&` borrows may coexist;
- exactly one `T&` borrow may exist at a time;
- a shared borrow and a mutable borrow of the same value cannot overlap.

Multiple shared borrows are fine:

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    const int& second = value;
    std::println("{}", first + second);
    return 0;
}
```

Output:

```text
10
```

But if a shared borrow is already live, a mutable borrow of that same value is rejected:

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    int& second = value;
    return first + second;
}
```

Compiler output:

```text
shared_and_mut_fail.scpp:6:5: error: cannot mutably borrow 'value': it is already borrowed
 6 |     int& second = value;
   |     ^
```

The same rule also rejects a second `T&` borrow of the same object.

This restriction is what keeps mutation disciplined: code can either have shared readers, or one mutable writer, but not both at once.

## Reborrowing from an existing reference

A new reference formed *from another reference* is a reborrow.

This is where scpp's current rules become more precise than a simple “scope of the block” story. If a mutable lender creates a child borrow, scpp tracks whether that child borrow is still *live* based on its last possible use.

While that child borrow is live:

- reads through the lender are still allowed;
- writes through the lender are rejected;
- further reborrows from that same lender are rejected.

A read through the lender is okay:

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    std::println("{}", lender + child);
    return 0;
}
```

Output:

```text
10
```

But a write through the lender while that child is still live is rejected:

```cpp
import std;

int main() {
    int value{1};
    int& first = value;
    int& second = first;
    first = 2;
    std::println("{} {}", first, second);
    return 0;
}
```

Compiler output:

```text
reborrow_write_lender_fail.scpp:7:5: error: cannot write through 'first' while a nested reborrow derived from it is still live
 7 |     first = 2;
   |     ^
```

Likewise, trying to create `int& third = first;` at that point is rejected with:

```text
reborrow_further_reborrow_fail.scpp:7:5: error: cannot form another reborrow from 'first' while a nested reborrow derived from it is still live
 7 |     int& third = first;
   |     ^
```

Once the child borrow's last use is over, the lender becomes writable again even before the block ends:

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    int before = child;
    lender = 7;
    int& second = lender;
    second = second + 1;
    std::println("{} {}", before, second);
    return 0;
}
```

Output:

```text
5 8
```

That `lender = 7;` line is accepted because `child` was already used for the last time at `int before = child;`. scpp is checking liveness by last use, not only by lexical scope.

## References must stay valid

A reference must never outlive what it refers to.

Today, scpp v0.1 enforces this conservatively for function returns: if a function returns a reference, the compiler must be able to infer that returned reference from an input reference parameter. So a function cannot manufacture a reference from a local object and return it.

```cpp
import std;

const std::string& bad() {
    std::string local{"oops"};
    return local;
}

int main() {
    const std::string& ref = bad();
    std::println("{}", ref.c_str());
    return 0;
}
```

Compiler output:

```text
return_local_ref_fail.scpp:3:1: error: function 'bad' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- refactor to take a single reference parameter, or return by value/std::unique_ptr instead
 3 | const std::string& bad() {
   | ^
```

The straightforward fix is to return the owned value itself:

```cpp
import std;

std::string make_title() {
    std::string local{"scpp"};
    return local;
}

int main() {
    std::string title{make_title()};
    std::println("{}", title.c_str());
    return 0;
}
```

Output:

```text
scpp
```

## The rules of references

So far, the working rules are:

- `const T&` borrows a value without taking ownership, and direct writes through it are rejected;
- `T&` also borrows without taking ownership, but allows mutation;
- a value may have either any number of shared borrows or one mutable borrow;
- a live reborrow blocks writes and further reborrows through its lender, but still allows reads through that lender;
- reborrow liveness is based on last use, not just on where a block ends;
- references must stay valid, and today scpp only accepts reference returns when it can infer the lifetime from an input reference.

The next section applies the same borrowing model to non-owning views over ranges of elements.

---

[← Previous: What Is Ownership?](ch04-01-what-is-ownership.md) · [Table of Contents](README.md) · [Next: `std::span` and Other Non-Owning Views →](ch04-03-std-span-and-other-non-owning-views.md)
