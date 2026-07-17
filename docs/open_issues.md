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

The previously-open question about unchecked integer-to-enum casts was resolved
by the specification in [docs/spec/en/09-enumeration-conversions.md](spec/en/09-enumeration-conversions.md).
