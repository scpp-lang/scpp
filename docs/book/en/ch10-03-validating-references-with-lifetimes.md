# Validating References with Lifetimes

The previous section showed how generic APIs describe *which operations* a type
must support. References add one more question: if a generic API accepts or
returns borrows, how does it describe *where those borrows may come from*?

In scpp today, that job belongs to `[[scpp::lifetime(...)]]` annotations. They
let generic functions and concept probes describe how reference parameters
relate to each other, and they let certain callback-style APIs express “this
reference is fresh and only valid for the duration of the call”.

For each runnable example below, save the file as `lifetimes.scpp`, then build
and run it like this:

```sh
scpp lifetimes.scpp -o lifetimes
./lifetimes
```

## A generic function can name which input reference its return value comes from

When a generic function returns a reference, a lifetime annotation can say which
input reference that result is tied to.

```cpp
import std;

template<typename T>
const T& keep_left(
    const T& left [[scpp::lifetime(a)]],
    const T& right [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return left;
}

int main() {
    int first = 31;
    int second = 8;
    const int& kept = keep_left(first, second);
    int value = kept;
    std::println("{}", value);
    return 0;
}
```

Output:

```text
31
```

The important part is not the concrete `int`, but the relation: the returned
reference is explicitly tied to the parameter in group `a`, not to the one in
group `b`.

## A concept probe can check lifetime relationships between several references

Concepts are not limited to checking names and return types of operations. A
probe can also describe how reference parameters must relate to each other.

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

class Adder {
public:
    virtual ~Adder() { return; }

public:
    int add(Token& a [[scpp::lifetime(shared)]], Token& b [[scpp::lifetime(shared)]]) const {
        return a.value + b.value;
    }
};

template<typename T>
concept AddsPair = requires(T c, Token& x [[scpp::lifetime(p)]], Token& y [[scpp::lifetime(p)]]) {
    { c.add(x, y) } -> std::same_as<int>;
};

int use_it(const AddsPair auto& adder, Token& left, Token& right) {
    return adder.add(left, right);
}

int main() {
    Adder adder{};
    Token first{};
    first.value = 3;
    Token second{};
    second.value = 4;
    int sum = use_it(adder, first, second);
    std::println("{}", sum);
    return 0;
}
```

Output:

```text
7
```

The probe groups `x` and `y` together under `p`. `Adder::add()` uses a
different spelling, `shared`, but that is fine: the point is that both
parameters belong to one shared group in both declarations.

## `[[scpp::lifetime(any)]]` lets a callback accept a freshly created borrow

Some generic APIs create a value inside their own body and pass a reference to a
callback. In that pattern, the callback cannot name the concrete lifetime ahead
of time, because the callee invents it.

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

template<typename T>
concept AcceptsToken = requires(T callback, Token& tok [[scpp::lifetime(any)]]) {
    { callback(tok) } -> std::same_as<void>;
};

void with_fresh_token(AcceptsToken auto&& callback) {
    Token tok{};
    tok.value = 42;
    callback(tok);
    return;
}

int main() {
    int result = 0;
    with_fresh_token([&result](Token& tok [[scpp::lifetime(any)]]) {
        result = tok.value;
        return;
    });
    std::println("{}", result);
    return 0;
}
```

Output:

```text
42
```

The reserved group name `any` means “a fresh lifetime chosen for this call”.
That lets the wrapper pass a reference to a local `Token`, while the callback is
still prevented from treating that borrow as though it belonged to some longer
lived named group.

## The working rules for lifetimes in generic APIs

So far, the practical rules are:

- use `[[scpp::lifetime(name)]]` on reference parameters when a generic API needs
  to describe how borrows relate across a call boundary;
- use a matching return annotation when a returned reference is tied to one
  particular input group;
- concept probes may check lifetime grouping relations, not just operation names
  and return types;
- those group names are declaration-local, so a probe's `p` does not need to
  reuse the callee's own spelling;
- use `[[scpp::lifetime(any)]]` for callback-style APIs that hand out a fresh
  short-lived borrow created inside the callee.

That completes Chapter 10's opening toolkit: generic definitions, concept-based
operation requirements, and lifetime annotations for references. The next
chapter builds on that foundation with closures, where local callback objects
meet captures, concepts, and lifetimes in ordinary user code.

---

[← Previous: Defining Shared Requirements with Concepts](ch10-02-defining-shared-requirements-with-concepts.md) · [Table of Contents](README.md) · [Next: Closures and Captures →](ch11-01-closures-and-captures.md)
