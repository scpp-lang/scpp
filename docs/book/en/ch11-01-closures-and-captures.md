# Closures and Captures

After generic functions, concepts, and lifetimes, the next practical step is
local callback code.

In scpp today, closures reuse ordinary C++ lambda syntax. The important mental
model is simple: a closure is a small anonymous object, and its capture list
describes what data that object stores or borrows.

For each runnable example below, save the file as `closures.scpp`, then build
and run it like this:

```sh
scpp closures.scpp -o closures
./closures
```

## A closure can be passed to a generic callback parameter

The most common use is to hand a short local action to another function.

```cpp
import std;

template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

void for_each_doubled(std::span<int> s, IntConsumer auto&& f) {
    int i = 0;
    while (i < s.size) {
        f(s[i] * 2);
        i = i + 1;
    }
    return;
}

int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    int sum = 0;
    for_each_doubled(s, [&sum](int x) { sum = sum + x; });
    std::println("{}", sum);
    return 0;
}
```

Output:

```text
12
```

The closure stores a mutable-reference capture of `sum`, so each callback
invocation updates the same outer variable.

## Mixed capture forms let one closure borrow some values and copy others

Capture lists are explicit about which outer names become owned fields and which
become borrowed reference fields.

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    int first = apply([&, a](int z) -> int { return a + b + z; }, 3);
    int second = apply([=, &b](int z) -> int { return a + b + z; }, 3);
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

Output:

```text
18
18
```

`[&, a]` means “borrow everything else, but copy `a` by value”. `[=, &b]`
means the opposite: “copy everything else, but borrow `b` by reference”.

## Methods must capture `this` explicitly

Inside a method, a closure may call back into the receiver, but `this` must be
named explicitly in the capture list.

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

class Multiplier {
public:
    virtual ~Multiplier() { return; }

private:
    int factor_{};

public:
    Multiplier(int factor) : factor_{factor} {
        return;
    }

    int scale(int x) const {
        return x * this->factor_;
    }

    int use_closure(int z) {
        return apply([this](int value) -> int { return this->scale(value); }, z);
    }
};

int main() {
    Multiplier m{7};
    int answer = m.use_closure(3);
    std::println("{}", answer);
    return 0;
}
```

Output:

```text
21
```

That explicit `[this]` keeps the lifetime edge visible. A closure inside a
method does not get an implicit escape hatch around borrowing rules.

## The working model for closures in today's scpp

So far, the practical rules are:

- a closure is best understood as a small anonymous object;
- by-value captures become ordinary owned fields of that object;
- by-reference captures become borrowed reference fields, so closure values
  follow the same lifetime reasoning as other reference-holding objects;
- passing a closure to another function usually works through an ordinary
  generic parameter constrained by a concept;
- inside methods, `this` must be captured explicitly.

That is enough to write ordinary callback-style code with today's scpp. The next
section can stay in the same area and add generic lambdas.

---

[← Previous: Validating References with Lifetimes](ch10-03-validating-references-with-lifetimes.md) · [Table of Contents](README.md)
