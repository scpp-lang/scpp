# Open design issues

This file records language and design questions that have come up in real work
but are deliberately deferred for later consideration.

It is **not** the place for "feature not implemented yet" tracking. That is what
`docs/missings.md` is for. This file is for cases where the implementation
question is blocked on a still-unsettled design decision.

## Thread-safety structural derivation ignores inherited base subobjects

The current thread-safety structural-derivation rules in
[docs/spec/en/04-thread-safety-properties.md](spec/en/04-thread-safety-properties.md)
determine a class's `thread-movable` and `thread-shareable` properties only
from that class's own declared non-static data members. They do not yet account
for inherited ordinary base-class subobjects.

As a result, a derived ordinary class may currently be judged
`thread-shareable` or `thread-movable` even when one of its inherited ordinary
base-class subobjects would prevent that result. This is a pre-existing gap in
the general thread-safety rules, not an interface-specific issue; interface
bases under [docs/spec/en/11-inheritance-and-interfaces.md](spec/en/11-inheritance-and-interfaces.md)
are unaffected because they contribute no non-static data members.

Future work should extend the structural-derivation rules to include inherited
ordinary base-class subobjects when determining a derived class's own
thread-safety properties.

## File-scope globals currently keep C++'s cross-file dynamic-initialization-order hazard

PR [#250](https://github.com/scpp-lang/scpp/pull/250) intentionally kept newly
working file-scope / namespace-scope variable declarations instead of reverting
them, because global constants such as `constexpr int X = ...;` are a genuinely
wanted use case, and `alignas` may also need to apply to such globals. However,
this also reintroduces the classic C++ "static initialization order fiasco" for
globals that require true dynamic initialization.

Within one translation unit / source file, C++ defines dynamic initialization
order by declaration order. Across different translation units / source files,
the relative order of dynamic initialization is unspecified. That means a
global in one file whose constructor or runtime initializer depends on another
dynamically-initialized global in a different file may observe that dependency
either before or after it has been initialized.

Globals with no initializer, or with a genuine constant-expression initializer
(that is, initialization that can be completed deterministically without
cross-file runtime ordering), are not affected by this hazard and are the
primary intended use case that is fine to keep unrestricted for now.

By contrast, globals that require non-constant, runtime-computed dynamic
initialization are potentially affected when they have cross-file dependencies
on other dynamically-initialized globals. scpp does not currently restrict or
diagnose that pattern yet. The behavior intentionally remains available for now
via PR [#250](https://github.com/scpp-lang/scpp/pull/250); as of this writing,
there is not yet a separate open or merged
`test-agent/global-vars-alignas-coverage` blackbox-coverage PR to cross-link
here.

Future work should address this explicitly. Because scpp is a whole-program AOT
compiler rather than a traditional separate-compilation-plus-linking C++ toolchain,
it may be possible to statically detect and diagnose problematic cross-file
dynamic-initialization-order dependencies at compile time, instead of either
blanket-banning all file-scope globals or silently inheriting C++'s full
unsafe/unspecified behavior.

## Whether `const T&` should continue to permit implicit materialization through converting constructors

Under [§6.2(11)](spec/en/02-ownership-and-move.md) and
[§6.6](spec/en/02-ownership-and-move.md), a `const T&` binding may currently
materialize a temporary `T` not only from an argument that is already a value
of type `T` under §6.2(11.1), but also from an expression that implicitly
selects a single-argument constructor of `T` under §6.2(11.2). For example, a
call that appears to pass a string literal or `const char*` through by
reference may in fact construct a fresh temporary `std::string` and bind the
reference to that temporary:

```cpp
void read_text(const std::string&);

read_text("hi");                // currently OK: §6.2(11.2), §6.6
read_text(std::string{"hi"});   // explicit spelling of the same construction
```

This is convenient and familiar from C++, but it is also easy to misread. A
reader skimming the call site gets no visual cue that a conversion and class-
type construction are happening at all. Code that looks like it is merely
passing a pointer-like or view-like value by reference may instead be creating
an owning object of a different, potentially expensive, type behind the scenes.

The open design question is whether scpp should eventually forbid all such
implicit class-type conversions and require the conversion to be written
explicitly at the call site instead. In that direction, §6.2(11.2) would be
removed and only §6.2(11.1) would remain; then every reference or value
binding of class type would require the argument to already be of the exact
type, whether as an existing object or as a fresh value under
[§6.6](spec/en/02-ownership-and-move.md) and
[§6.7](spec/en/02-ownership-and-move.md).

This surfaced during the self-hosting bootstrap effort to compile scpp's own
`src/ast.cppm` with scpp itself. In that work, `return "operator_equal";` from
a function returning `std::string` is correctly rejected today under
[§6.7](spec/en/02-ownership-and-move.md), because by-value return uses the
existing fresh-value rules and does not perform the constructor-based
materialization that [§6.2(11)](spec/en/02-ownership-and-move.md) currently
permits for `const T&` binding. That leaves an asymmetry between reference-
binding and by-value contexts that is still unresolved: later work may either
extend such implicit materialization symmetrically to by-value cases as well,
or remove it from reference-binding in favor of full consistency and
explicitness.

The previously-open question about unchecked integer-to-enum casts was resolved
by the specification in [docs/spec/en/09-enumeration-conversions.md](spec/en/09-enumeration-conversions.md).
