# Paths for Referring to Items in the Module Tree

The previous section reached every item through a path: `mathlib::sum_of_squares`,
`mathlib::trig::sin_deg`, `stats::sum_of_squares_twice`. Each was written out in
full, at the call site, every time. This section looks at what a path actually
is, how a module's own dotted name shapes it, and how little shorthand scpp
gives you for skipping any of it.

A path is nothing more than an item's namespace nesting, joined with `::`,
ending in its own name. It does not depend on which file declares the item, and
it is a separate thing from the module's own dotted import name -- the two
use different separators for a reason, as the first section below shows.

Every example below lives in one package.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "mathlib-app"
version = "0.1.0"

[[bin]]
name = "app"
sources = ["src/*.scpp"]
```

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## A module's dotted name maps segment for segment onto a path

[Control Scope and Privacy with Modules](ch07-02-control-scope-and-privacy-with-modules.md)
mentioned in passing that a module's own name can have several dot-separated
segments, such as `mathlib.trig`, and that each segment maps one-for-one onto a
`::`-separated namespace segment. Every module up to this point used a
single-segment name, so this never came up directly. A real, two-segment
module name makes it concrete.

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

Output:

```text
30
```

`import mathlib.trig;` names the module the way it names itself, with a dot.
Reaching what it exports still uses `::`, exactly as before --
`mathlib::trig::sin_deg`. The module's own two dot-separated segments,
`mathlib` and `trig`, become the two required `::`-separated namespace
segments, `mathlib` and `trig`. This is the same namespace-matching rule from
the previous section, only the module's own name now has more than one
segment for it to match.

The rule still holds exactly: a namespace one segment short of the module's
own name is rejected, the same way a missing namespace was rejected before.

```cpp
export module mathlib.trig;

namespace mathlib {
    export int sin_deg(int x) {
        return x;
    }
}
```

Compiler output:

```text
src/trig.scpp:4:12: error: exported function 'mathlib::sin_deg' must be declared in namespace matching module 'mathlib.trig' -- ch11 §11.5
 4 |     export int sin_deg(int x) {
   |            ^
```

`namespace mathlib` only supplies the first of the two required segments, so
it is treated exactly like any other namespace that fails to match: the same
error as an unrelated namespace, or no namespace at all.

## Only a name declared in that same exact namespace can skip its path

A namespace can still nest deeper than a module's own name requires -- that
part of the rule from the previous section is unchanged. What is new here is
what an unqualified call can and cannot reach once namespaces start nesting.

`src/trig.scpp`, with a helper and a further-nested namespace added:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    int double_it(int x) {
        return x * 2;
    }

    export int sin_deg(int x) {
        return double_it(x);
    }
}

namespace mathlib::trig::deg {
    export int right_angle() {
        return mathlib::trig::double_it(45);
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(400));
    std::println("{}", mathlib::trig::deg::right_angle());
    return 0;
}
```

Output:

```text
800
90
```

`sin_deg` calls `double_it` with no path at all, and that works, because both
are declared directly inside the same `namespace mathlib::trig { ... }` block.
`right_angle`, declared one level deeper in `mathlib::trig::deg`, calls the
same `double_it` through its full path, `mathlib::trig::double_it`, even
though `mathlib::trig` is its own directly enclosing namespace. Dropping that
path and calling `double_it` bare from inside `mathlib::trig::deg` does not
work:

```cpp
namespace mathlib::trig::deg {
    export int right_angle() {
        return double_it(45);
    }
}
```

Compiler output:

```text
src/trig.scpp:15:16: error: call to unknown function 'double_it'
 15 |         return double_it(45);
    |                ^
```

An unqualified call only reaches a name declared directly in that exact same
namespace (or, as every earlier example's plain functions showed, declared in
no namespace at all). It does not climb outward through enclosing namespaces
the way looking up a variable in nested blocks does. Crossing into any other
namespace -- even the one directly wrapping the namespace you are writing in
right now -- always means writing out the full path.

## A leading `::` starts the search at the outermost scope

A path can also begin with a leading `::`. This does not change what a fully
written-out path like `mathlib::trig::sin_deg` reaches -- it still names the
same item -- but it does guarantee that the search starts from the outermost
scope rather than considering anything else already in scope at the call
site. The difference only shows up when something else in scope could
otherwise be reached by the same bare name.

```cpp
import std;

int count() {
    return 100;
}

int main() {
    int count = 7;
    std::println("{}", count);
    std::println("{}", ::count());
    return 0;
}
```

Output:

```text
7
100
```

Both `count`s are visible inside `main`: a local variable and, without the
leading `::`, an ordinary function of the same name would simply be shadowed
by it. Writing `count` bare reaches the local variable. Writing `::count()`
skips straight past it and reaches the function declared at the outermost
scope. The same leading `::` works in front of a full path, with the same
meaning -- start at the outermost scope, then follow the rest of the path
exactly as written:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", ::mathlib::trig::sin_deg(400));
    std::println("{}", ::mathlib::trig::deg::right_angle());
    return 0;
}
```

Output:

```text
800
90
```

Nothing in scope at `main`'s call sites could have been confused with
`mathlib::trig::sin_deg` or `mathlib::trig::deg::right_angle` here, so the
leading `::` makes no difference to the result. It is available on any path,
not only ones that happen to need it.

## A path still cannot reach what was never exported

None of this reopens the privacy rules from the previous section. A path that
exactly matches a declaration's own namespace nesting still fails if that
declaration was never `export`-ed -- writing the path correctly is not enough
on its own.

```cpp
import std;
import mathlib.trig;

int main() {
    return mathlib::trig::double_it(5);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::double_it'
 5 |     return mathlib::trig::double_it(5);
   |            ^
```

`double_it` really does live at `mathlib::trig::double_it` -- that is the
exact path used inside `trig.scpp` itself in the section above -- but it was
never exported, so no path reaches it from `main.scpp`. A path only ever
describes where something lives; whether it can be followed from outside the
module still depends entirely on `export`.

## The path rules so far

- a path is an item's namespace nesting joined with `::`, ending in its own
  name -- independent of which file declares it;
- a module's own dotted name maps segment for segment onto the `::`-separated
  namespace its exports must live in, however many segments it has;
- an unqualified call only reaches a name declared in that exact same
  namespace, or declared in no namespace at all -- reaching any other
  namespace, including one directly enclosing the caller, needs the full
  path;
- a leading `::` starts a path at the outermost scope, ahead of anything else
  already in scope at the call site;
- a path can only ever reach a declaration that is both correctly placed and
  `export`-ed -- getting the path right does not, on its own, cross the
  privacy boundary from the previous section.

Every path in this section was still written out by hand, in full, in every
file that needed it. The next section returns to `import` itself, and to how
it and a path's own qualified name work together in practice.

---

[← Previous: Control Scope and Privacy with Modules](ch07-02-control-scope-and-privacy-with-modules.md) · [Table of Contents](README.md)
