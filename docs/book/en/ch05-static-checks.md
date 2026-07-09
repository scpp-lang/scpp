# Static Checks (the soundness core)

Chapter 4 explained *which kinds of values* scpp distinguishes. This chapter is
about the other half of the language: **what the compiler proves about those
values in ordinary code**.

The shortest accurate summary is this:

- safe scpp code is checked by default;
- `[[scpp::unsafe]]` does **not** turn that checking off;
- it only re-opens a small, explicit set of operations the checker cannot prove
  on its own.

If Rust's ownership model is the right intuition, keep it. scpp is trying to do
that job while reusing familiar C++ surface syntax.

## 5.1 Ownership & Move

Every non-trivial value in scpp has one owner at a time. For `class` values and
library types such as `std::unique_ptr<T>`, ownership moves explicitly with
`std::move(x)`.

After a move:

- the destination becomes the new owner;
- the source binding enters the **moved-out** state;
- reading the moved-out binding again is a compile error until it is assigned a
  fresh value.

That is why scpp can make destruction predictable: values still initialized at
scope exit are destroyed; moved-out values are not.

```cpp
import std;
extern "C" int puts(const char* s);

int main() {
    std::unique_ptr<int> owner = std::make_unique<int>(9);
    std::unique_ptr<int> next = std::move(owner);

    if (*next == 9) {
        [[scpp::unsafe]] {
            puts("ownership moved exactly once");
        }
    }
    return 0;
}
```

Output:

```text
ownership moved exactly once
```

Two points are easy to miss if you are coming from C++.

First, `std::move` is not “just a cast that maybe enables an overload”. In
scpp it is part of the ownership model: using it means “this binding is no
longer the owner after this point”.

Second, `struct` values from [§4.1](ch04-struct-vs-class.md) are different.
Because they are trivial by construction, they copy by value instead of
participating in move tracking.

## 5.2 Borrow & Aliasing

scpp's borrowing rule is the same fundamental rule Rust users expect:
**alias XOR mutability**.

At any moment, a place may have either:

- any number of shared borrows (`const T&`), or
- exactly one mutable borrow (`T&`),

but never both at the same time.

The rule is enforced on root places, not on “whatever syntax happened to produce
a reference”. In v0.1 that means the checker is deliberately conservative:
borrowing `obj.left` and `obj.right` still counts as borrowing the same root
`obj`. This loses some precision, but it keeps the model simple and sound.

## 5.3 Lifetime

A borrow must not outlive the value it refers to. That is the lifetime rule in
one sentence.

The important usability point is that v0.1 already does **last-use-based**
lifetime reasoning inside a function. A borrow ends after its last use, not only
at the closing brace of the lexical scope that contains it.

```cpp
extern "C" int puts(const char* s);

int main() {
    int value = 5;
    const int& left = value;
    const int& right = value;
    int total = left + right;

    value = 9;

    [[scpp::unsafe]] {
        if (total == 10 && value == 9) {
            puts("borrows ended after their last use");
        }
    }
    return 0;
}
```

Output:

```text
borrows ended after their last use
```

`left` and `right` are still *in scope* when `value = 9` runs, but they are no
longer *live*. That distinction is what lets the compiler accept ordinary code
without waiting for the end of the whole block.

### Cross-function lifetimes

For returned references, the book's full design uses `[[scpp::lifetime(name)]]`
to name parameter groups when needed. That remains the long-term direction, but
the **current compiler** still implements only the existing small subset:

- one eligible reference parameter, or
- the usual implicit `this` case for methods.

Parameter-side `[[scpp::lifetime(name)]]` attributes are currently parsed for
forward compatibility, but the full multi-group return-group mechanism is not
enforced yet.

## 5.4 Initialization

scpp has no “maybe uninitialized local” state. A local or member with no
explicit initializer starts from a well-defined zero value.

- integers become `0`;
- `bool` becomes `false`;
- floating-point values become `0.0`;
- raw pointers become `nullptr`;
- aggregates are zeroed field-by-field.

```cpp
extern "C" int puts(const char* s);

struct Totals {
    int ok;
    int bad;
};

int main() {
    Totals totals;

    [[scpp::unsafe]] {
        if (totals.ok == 0 && totals.bad == 0) {
            puts("locals start from zero");
        }
    }
    return 0;
}
```

Output:

```text
locals start from zero
```

That choice is deliberately different from both Rust and C++: Rust rejects
uninitialized reads statically, and C++ permits undefined behavior here. scpp
instead makes the default value defined and predictable.

## 5.5 Prohibited (unless in `[[scpp::unsafe]]`)

`[[scpp::unsafe]]` is narrow. It re-allows a fixed list of operations; it does
not suspend the ownership or lifetime checker.

The main gated operations are:

- raw pointer dereference and pointer arithmetic;
- `reinterpret_cast` and incompatible C-style casts;
- `union` member access;
- raw `new` / `delete`;
- mutable global/static access;
- calling an `extern "C"` declaration with no body;
- calling a function declared `[[scpp::unsafe]]`;
- calling through an `[[scpp::unsafe]]` function pointer.

```cpp
extern "C" int puts(const char* s);

int main() {
    int value = 42;
    int* p = &value;

    [[scpp::unsafe]] {
        if (*p == 42) {
            puts("unsafe is narrow and explicit");
        }
    }
    return 0;
}
```

Output:

```text
unsafe is narrow and explicit
```

That example is representative of the whole design. Creating the raw pointer
with `&value` is fine in safe code. The moment you want to *trust* it and
dereference it, you must say so locally.

Just as important is what `[[scpp::unsafe]]` does **not** do:

- it does not allow use-after-move;
- it does not permit conflicting borrows;
- it does not permit dangling references;
- it does not reintroduce uninitialized storage.

Those checks keep running everywhere.

## 5.6 Recoverable Errors: `std::expected<T, E>`

scpp has no exceptions. The intended recoverable-error path is
`std::expected<T, E>`.

Design-wise, that means:

- bug/contract-violation cases abort;
- recoverable failures travel as ordinary return values;
- ignoring an `expected` result is a hard error, not merely a warning.

**Current compiler status:** this design is settled, but `std::expected` is not
implemented in the current compiler/stdlib yet. Today, mentioning it is still
rejected.

## 5.7 Address-of (`&x`) and Raw Pointers

One of scpp's most important boundary decisions is that **forming** a raw
pointer is safe, but **trusting** it is not.

- `&expr` is always allowed and yields `T*` or `const T*` depending on the
  place you took the address of.
- `T* -> const T*` is the one-way implicit conversion you expect.
- Writing through a `const T*` is an ordinary type error, even inside
  `[[scpp::unsafe]]`.
- Dereferencing a raw pointer is gated by [§5.5](#55-prohibited-unless-in-scppunsafe).

That split is what lets safe code prepare out-parameters for C APIs without
pretending raw pointers are themselves tracked borrow types.

## 5.8 Integer Overflow

scpp checks `+`, `-`, and `*` for integer overflow by default.

- In ordinary code, overflow aborts.
- Inside `[[scpp::unsafe]]`, those operations become unchecked but still
  defined to wrap; they do not become C++-style undefined behavior.
- Division by zero, modulo by zero, and `INT_MIN / -1` remain aborting cases in
  every context.

This belongs next to `span` bounds checks conceptually: both are runtime checks
inserted by the language, and both are skipped inside `[[scpp::unsafe]]`
because skipping them cannot corrupt the compiler's static ownership
bookkeeping.

## 5.9 Methods and `this`

A method is checked as though `this` were an implicit reference parameter.

- a non-`const` method takes an exclusive borrow of the receiver;
- a `const` method takes a shared borrow of the receiver;
- those borrows compose with the same aliasing rules as ordinary references.

That is why “holding a borrow of one public field, then calling a mutating
method on the same object” is rejected in v0.1: both actions touch the same
root object.

## 5.10 Function Overloading

scpp keeps ordinary C++ overloading syntax, but it makes the resolution rule far
less magical than real C++ by first removing the thing that makes C++ overload
sets so surprising: implicit scalar conversions.

The core rule is simple: **overloads differ by parameter list, never by return
type, and a candidate is viable only on exact type match**.

That one choice explains most of the section.

- **Exact type match only.** Because [§6](ch06-safe-subset.md) forbids implicit
  conversions between distinct scpp scalar types, overload resolution does not
  need C++'s ranking ladder of Exact Match > Promotion > Conversion > .... For a
  call to be viable, each argument type must already be identical to the
  corresponding parameter type. If nothing matches, the program needs an
  explicit cast at the call site.
- **This is deliberately simpler than real C++.** In C++, promotion targets are
  specifically `int`/`unsigned int`/`double`, not “the nearest wider type”, and
  two candidates that are both merely conversion-tier can still be ambiguous.
  scpp avoids that whole category by refusing to guess in the first place.
- **By-value versus by-reference is its own axis.** `f(T)`, `f(T&)`, and
  `f(const T&)` are three distinct overloads. In scpp this matters because they
  mean different ownership stories: take ownership, mutable borrow, or shared
  borrow. A bare named lvalue is only viable for the reference forms; a fresh
  value such as `std::move(x)` or a temporary is what makes the by-value form
  viable. If both `T&` and `const T&` match a mutable lvalue, `T&` wins, like
  ordinary C++.
- **Scope still works like C++.** An inner declaration hides the whole outer
  overload set. A `using foo::bar;` declaration imports every visible overload
  of `bar`, not just one.
- **There is no ADL complication in v0.1.** The candidate set for an
  unqualified call is exactly what ordinary lexical lookup plus `using`
  declarations finds.
- **Taking the address of an overloaded name is target-type-driven.** The target
  function-pointer type chooses the one overload whose parameter types and
  return type match exactly. That rule matters again in [§5.16](#516-function-pointers),
  where pointer types themselves can be `[[scpp::unsafe]]`-qualified.

So the practical reading rule is: if you can tell which overload matches by
looking only at the argument types already written in the source, you are using
overloading the way scpp intends.


## 5.11 Generic Functions and Concepts

Generic functions are scpp's v0.1 answer to compile-time polymorphism. The
surface syntax is ordinary C++20 `concept`/`requires` syntax, but the semantics
are intentionally tuned toward “checked generic code” rather than “template
metaprogramming that type-checks later”.

Three rules carry most of the weight.

- **Concept satisfaction is structural, not nominal.** A type satisfies a
  concept because it has the required operations; there is no Rust-style
  `impl Trait for Type` declaration that opt-ins nominally. This was a
  deliberate design choice, not an omission: Rust's nominal trait model avoids
  “accidental satisfaction”, but real C++ offers no existing `impl`-like syntax
  for scpp to reuse, and inventing one would violate the book's erasure
  principle from [ch00](ch00-design-philosophy.md). So scpp keeps real C++'s
  structural model unless a future C++ standard itself gives a nominal form to
  borrow.
- **A constrained generic body is checked once, at its own definition.** Inside
  that body, the parameter's type is treated abstractly, and only operations
  promised by the concept's `requires`-expression are legal. That is a real
  semantic difference from ordinary C++ templates: scpp does not want the
  meaning of the body to depend on which later instantiation happened to trigger
  it, so it rejects “this template only works for some instantiations” style
  body checking and prefers Rust-like “the bound must justify the body” rules.
- **Every instantiation is still monomorphized.** There is no vtable and no
  runtime dispatch cost here; generic functions are still zero-cost abstractions.

A few design details matter in practice.

- **A concept is optional in the surface syntax.** Design-wise, a bare generic
  parameter means “the type is opaque”: you may move it, store it, pass it
  through, and return it, but you may not call methods on it or apply operators
  that require more knowledge. Naming a concept unlocks only what that concept
  guarantees.
- **Current compiler status.** The long-term design includes abbreviated bare
  `auto` parameters and concept-constrained parameter packs, but the current
  compiler still rejects those two forms. Today, the reliably implemented
  generic-function spelling is the full header form (`template<typename T>`,
  optionally with `requires`), which is why most working examples use it.
- **Parameter packs split into two cases.** In the design, an abbreviated pack
  such as `Concept auto&... args` is only meant to be consumed through a fold
  expression, because a fold has one meaning for every pack length, including
  zero. The full-header form (`template<typename... Args>`) can additionally
  forward `args...` into another call or construction site. What scpp still does
  not want is recursive “peel one argument and recurse on the rest” checking
  inside a function body; that pushes reasoning back toward instantiation-time
  behavior.
- **The full header form carries the extra expressive power.** Multiple type
  parameters, explicit non-deduced template arguments, and “return-type-only”
  generic functions all live there. That is the form later sections rely on when
  they need more than “one constrained parameter goes here”.

So if you are a C++ reader, the right mental shift is: *the syntax looks like
C++20 concepts, but the body-checking contract is closer to Rust trait bounds
than to traditional template instantiation folklore*.


## 5.12 Closures (Lambda Expressions)

Lambda expressions reuse C++ lambda syntax, but scpp explains them in the
simplest possible way: a lambda lowers to an anonymous compiler-synthesized
`class` with one field per capture and an `operator()` containing the body.

That is important because it means lambdas do **not** get a second, special
ownership model.

- **By-value captures become ordinary owned fields.** Moving the closure moves
  those fields; destroying the closure destroys them.
- **By-reference captures become reference-typed fields.** That makes the
  closure value itself lifetime-tracked, just like any other object that stores
  references.
- **The call operator is checked like any other method.** Receiver borrowing,
  parameter borrowing, return-value lifetime rules, and move checking all reuse
  the same machinery the rest of Chapter 5 already defined.
- **`this` and `*this` must be captured explicitly.** A bare `[=]` or `[&]`
  implicitly capturing `this` is rejected in scpp even though older C++ code
  often relied on it. scpp wants the lifetime edge to stay visible.

So the practical rule is: if you can explain a lambda as “just a small anonymous
`class`”, you are reasoning about it the same way the checker does.


## 5.13 Lifetime-Generic Parameters

This is the one place where ordinary named lifetime groups from [§5.3](#53-lifetime)
need a small extra idea.

The motivating use case is a library API shaped like Rust's `thread::scope`: the
callee creates some short-lived value *inside its own body* and wants to pass a
reference to that value into a callback. The callback's author could never have
named that lifetime ahead of time, because the callee chooses it.

scpp handles that pattern as a targeted extension of the existing
`[[scpp::lifetime(name)]]` model rather than by introducing Rust-style lifetime
parameters as a whole new syntactic category.

- **`[[scpp::lifetime(generic)]]` is a reserved group name.** It means “fresh,
  compiler-synthesized group that user code cannot otherwise name or unify with
  anything else”. Inside the callable's own body, that immediately limits what
  can happen: a `generic`-tagged reference can be used synchronously for reads,
  method calls, and forwarding to compatible parameters, but it cannot be stored
  into some other named group, returned, or hidden in longer-lived state.
- **A `requires`-expression may probe for that exact property.** When a concept's
  probe parameter is tagged `[[scpp::lifetime(generic)]]`, satisfying the
  concept means the callable's own matching parameter is itself lifetime-generic,
  not merely “some reference parameter that happens to accept this test call”.
  That is one new piece of concept-checking semantics attached to an already
  existing C++ grammar position.
- **Calls to such a callable get one targeted exemption.** Once the type system
  knows a callable parameter is lifetime-generic, the caller may pass it a
  reference whose lifetime is invented freshly inside the caller's own body,
  precisely because the two bullets above already guarantee the callee cannot
  stash that reference anywhere that would outlive the call.

This reaches the soundness pattern Rust expresses with higher-ranked trait
bounds, but it does so by extending scpp's existing named-group design instead
of reversing it.

**Current compiler status.** This caveat from the previous revision is worth
keeping: today the compiler still enforces only the older single-reference-
parameter / implicit-`this` subset from [§5.3](#53-lifetime). The
`[[scpp::lifetime(generic)]]` story here is the intended design, not yet a
fully implemented feature.


## 5.14 Generic Types

Generic `struct` and `class` types extend [§5.11](#511-generic-functions-and-concepts)'s
compile-time-polymorphism story from functions to type definitions.

The first rule to keep in mind is that Chapter 4's split still matters.

- **A generic `class` may leave its type parameter bare.** That means exactly
  what it meant for a generic function parameter: the type may be moved, stored,
  passed through, and returned, but methods and operators are unavailable unless
  a specific member adds a `requires` clause that unlocks them.
- **Those `requires` clauses are per member, not all-or-nothing for the whole
  type.** A constructor or method may demand `std::totally_ordered<T>` while a
  different method on the same generic type requires nothing beyond the bare
  baseline.
- **A generic `struct` may not leave its parameter bare.** `struct` triviality is
  a whole-type layout property, so a generic `struct` must constrain its type
  parameters strongly enough to guarantee that every field remains trivial.

The most important variadic rule is the one the earlier rewrite lost: **a
Tuple-like variadic generic type is supported only through recursive
inheritance, not by expanding a parameter pack directly into a member list.**
scpp makes that choice because no adopted C++ standard gives it a real syntax
for `Ts... members;`, and the book's erasure principle requires scpp source to
stay parseable by an ordinary C++ compiler.

So the supported pattern is this exact two-step shape:

```cpp
template<typename... Ts>
class Tuple;

template<>
class Tuple<> {};

template<typename Head, typename... Tail>
class Tuple<Head, Tail...> : private Tuple<Tail...> {
    Head head;
};
```

Read it as:

1. `Tuple<>` is the empty-pack base case;
2. `Tuple<Head, Tail...>` stores one field and inherits the rest.

That is the pattern a reader should actually copy when building a variadic
owning type in scpp today.

A few more boundaries matter:

- **Indexed access is meant to reuse base-class deduction.** The usual `get<I>`
  pattern works by targeting the correct recursive base specialization rather
  than by making the accessor itself recurse at runtime.
- **Non-type template parameters are supported for scalar types only.** That
  keeps the design out of C++'s much hairier class-typed structural-value world.
- **General specialization is still out of scope.** The supported variadic
  pattern is the fixed empty-pack / head-and-tail recursion above, not arbitrary
  specialization machinery.

So the implementation-level takeaway is concrete: if you want a working generic
`Tuple`-like type, write it as recursive inheritance with an empty-pack base
case.


## 5.15 Thread-Safety Structural Properties: `[[scpp::thread_movable]]`, `[[scpp::thread_shareable]]`, and `[[scpp::thread_movable_if(a, b)]]`

These attributes are scpp's structural “can this value cross a thread boundary
safely?” vocabulary.

They play the same role Rust's `Send`/`Sync` traits play conceptually:

- library APIs can require thread-movable or thread-shareable arguments;
- types may derive those properties structurally;
- attributes provide an explicit override point when the default structural rule
  is too weak or too strong.

The builtin predicates `scpp::is_thread_movable(T)` and
`scpp::is_thread_shareable(T)` let generic code query those properties.

## 5.16 Function Pointers

scpp reuses ordinary C/C++ function-pointer syntax, but it adds one crucial
piece of information to the type itself: a pointer-to-function type is either
`[[scpp::unsafe]]`-qualified or it is not.

That extra bit exists for the same reason the language distinguishes ordinary
functions from call sites that need local vouching. If taking the address of an
`[[scpp::unsafe]]` function or a bodyless `extern "C"` declaration produced an
ordinary plain function pointer, code could store it and call through it later,
silently bypassing the gate.

So the rule is: **the pointer type must remember whether calling through it is a
gated operation**.

- **The spelling is still real C++ syntax.** The marker sits on the pointer
  declarator, immediately after the `*`:
  ```cpp
  int (* [[scpp::unsafe]] up)(int, int);
  int (*                  sp)(int, int);
  ```
  That is an existing attribute position in C++ declarator grammar, so scpp
  does not need any new syntax here.
- **Address-taking determines the flavor automatically.** The address of an
  ordinary function has the plain pointer type. The address of an
  `[[scpp::unsafe]]` function, or of a bodyless `extern "C"` declaration, has the
  unsafe-qualified pointer type.
- **Conversion is one-way only: plain → unsafe-qualified.** Storing a plain
  function pointer in an unsafe-qualified slot is harmless because it only adds a
  stronger calling obligation. Going the other way would erase a real safety
  requirement, so it is rejected.
- **Calling through the pointer follows the pointer's own type.** A plain
  function pointer may be called freely. An unsafe-qualified one joins
  [§5.5](#55-prohibited-unless-in-scppunsafe) and may only be called inside an
  unsafe context.
- **Calling is not the same thing as raw-pointer dereference.** `fp(args)` and
  `(*fp)(args)` are both ordinary function-pointer-call syntax; neither triggers
  the separate raw-memory dereference rule from [§5.7](#57-address-of-x-and-raw-pointers).
- **Function pointers stay trivial.** Like any other raw pointer, they carry no
  compiler-tracked lifetime, are freely copyable, and may appear in a `struct`.
- **Overloaded names are selected by target type.** When taking the address of
  an overloaded function, the target pointer type chooses the one overload whose
  parameter types and return type match exactly.

Pointer-to-member-function syntax and functions returning function pointers are
still left out of v0.1. The important working rule for readers is simpler: if a
call would need local vouching by name, storing that function in a pointer does
not make the obligation disappear.


## 5.17 Dereference Operators on Classes

For a user-defined `class`, `*x` and `x->y` are not magic escape hatches. scpp
keeps them deliberately boring.

- **`*x` is just method-call sugar.** If the class declares `operator*()`, then
  `*x` is checked exactly like `x.operator*()`, using the same receiver-borrowing
  rules as any other method.
- **The returned reference follows the ordinary lifetime rules.** If
  `operator*()` returns `T&` or `const T&`, borrowing through `*x` records a
  borrow against `x`'s root object. Moving or reassigning `x` while that borrow
  is alive is rejected for the same reason any method-returned reference blocks a
  conflicting move.
- **`x->y` remains sugar for `(*x).y`.** Once `*x` works, member access through
  `->` follows automatically; no second ownership rule is needed.
- **There is no separately overloadable `operator->`.** scpp does not add real
  C++'s second protocol here, because the one `(*x).y` rewrite already gives the
  useful surface syntax.

So the safe way to read this feature is: it lets user-defined pointer-like
classes join an existing sugar path; it does not create a new unchecked route
around borrowing.


## 5.18 Type-Erased Call Wrappers: `std::function` and `std::move_only_function`

scpp supports two owning, type-erased callable wrappers:

- `std::function<Sig>` for copyable callable targets;
- `std::move_only_function<Sig>` for move-only ones.

The important design point is that these wrappers are **library-level
abstractions, not deep special compiler magic**. Once generic `class` templates
can express multiple parameters, partial specialization on a function-type
argument, generic constructors, and parameter packs in methods, the wrappers are
ordinary library code. The only builtin help they need is the same basic ability
real C++ template partial specialization already models: treating `R(Args...)`
as “return type plus parameter pack”.

A few semantic rules matter enough to say explicitly.

- **There is no empty/null sentinel state.** Unlike real C++'s `std::function`, a
  scpp callable wrapper always contains a valid target once it exists. If a
  program wants “maybe there is a callback”, that optionality belongs in the
  outer type, such as `std::optional<...>`.
- **Move follows the ordinary class rules.** Moving a wrapper moves the stored
  callable into the destination wrapper and leaves the source wrapper in the
  ordinary moved-out state. The source is not a special “still valid but empty”
  callable box.
- **`std::move_only_function` supports cv/ref-qualified signatures from the
  start.** A wrapper instantiated as `void() &` promises an lvalue-callable
  target; `void() &&` promises an rvalue-callable target; `void() const` promises
  a const-callable target.
- **If an extract/release operation exists, it must be consuming.** scpp does not
  want an API that peels the target out and leaves behind another hidden empty
  state.
- **Use erasure only when erasure is actually the point.** If a function merely
  wants to call the closure it was handed, [§5.11](#511-generic-functions-and-concepts)
  and [§5.12](#512-closures-lambda-expressions) are still the better tool: no
  erased dispatch, no hidden box, and full visibility of the concrete type.

So `std::function` and `std::move_only_function` are for the specific cases
where you really need one owning field or return type to hide several different
callable concrete types behind one stable signature.


## 5.19 `union` and `[[scpp::packed]]`

`union` exists for FFI and storage-overlay work, not as an everyday safe-data
structure. Carrying a `union` value is allowed in ordinary code, but reading or
writing one of its members requires `[[scpp::unsafe]]` because the compiler does
not track which member is currently active.

`[[scpp::packed]]` is the explicit “match a packed foreign layout” attribute for
`struct` and `union`. Use it when the outside ABI requires it, not as a general
performance tweak.

## What to keep in your head

If Chapter 5 feels dense, keep these three rules in mind and come back to the
rest as reference material:

1. **Ownership moves explicitly.**
2. **Borrows obey alias XOR mutability and lifetimes.**
3. **`[[scpp::unsafe]]` is narrow; it does not disable the checker.**

Almost every later chapter is just one consequence of those three rules.

---

[← Previous: Struct vs Class Semantics](ch04-struct-vs-class.md) · [Table of Contents](README.md) · [Next: The v0.1 Supported Subset →](ch06-safe-subset.md)
