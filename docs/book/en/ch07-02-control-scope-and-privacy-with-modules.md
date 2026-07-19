# Control Scope and Privacy with Modules

The previous section stayed at the package level: a manifest's `[[bin]]` and
`[[lib]]` tables, and how `scpp build` lets one package's binaries share
another target's module. This section moves down into the language itself:
once a binary can `import` a module, what does it actually get access to?

The short answer is: less than the whole file. A module's own source can
contain plenty of ordinary declarations that never leave it. Only two things
together make a declaration visible to an importer:

- it is marked `export`, and
- it is declared inside a namespace matching the module's own name.

Miss either one and the declaration stays private -- reachable from inside the
module's own file, invisible everywhere else.

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

A single `[[bin]]` target with a glob `sources` pattern is enough here: as
[Packages and Project Manifests](ch07-01-packages-and-project-manifests.md)
covered, a target's `sources` can name more than one file, and any of those
files can itself be a module that another file in the same target imports.
Nothing in this manifest needs to change for the rest of this section -- only
the `.scpp` files under `src/` do. Build and run each version with:

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## Only exported declarations are visible outside their module

`src/mathlib.scpp`:

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

Output:

```text
25
```

`square` is an ordinary function: no `export`, no enclosing namespace.
`sum_of_squares` is `export`-ed from inside `namespace mathlib`, matching the
module's own name, so `main.scpp` can reach it as `mathlib::sum_of_squares`.
`square` itself never crosses the module boundary -- `sum_of_squares` can still
call it locally, since this rule is only about what an *importer* can see, not
about what the module's own code can use internally.

Trying to reach `square` directly from the importer confirms this:

```cpp
import std;
import mathlib;

int main() {
    return square(5);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'square'
 5 |     return square(5);
   |            ^
```

From `main.scpp`'s point of view, `square` was never declared at all. This is
not an access-control error -- the name simply never entered this file's
scope, exactly as if it had never been written in the first place.

## An exported declaration must live in a namespace matching the module

`export` by itself was not enough above. `sum_of_squares` also had to be
declared inside `namespace mathlib { ... }` -- a namespace matching the
module's own name. `export` and "lives in the required namespace" are two
independent, both-mandatory conditions.

(A module's own name can have several dot-separated segments, such as
`mathlib.trig`; each segment maps one-for-one onto a `::`-separated namespace
segment. Every module in this section uses a single-segment name, so its
required namespace is just that one name -- the next section looks at
multi-segment module names and the paths they produce.)

Dropping the namespace entirely is rejected:

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

export int sum_of_squares(int a, int b) {
    return square(a) + square(b);
}
```

Compiler output:

```text
src/mathlib.scpp:7:8: error: exported function 'sum_of_squares' must be declared inside a namespace -- ch11 §11.5
 7 | export int sum_of_squares(int a, int b) {
   |        ^
```

So is exporting from an unrelated namespace:

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace geometry {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

Compiler output:

```text
src/mathlib.scpp:8:12: error: exported function 'geometry::sum_of_squares' must be declared in namespace matching module 'mathlib' -- ch11 §11.5
 8 |     export int sum_of_squares(int a, int b) {
   |            ^
```

`geometry` has nothing to do with this module's own name, `mathlib`, so it is
rejected exactly like the missing-namespace case above.

## Nesting deeper than the module's own name is still fine

The namespace requirement is a prefix match, not an exact match. A module
named `mathlib` requires its exported declarations to live inside `namespace
mathlib`, or inside any namespace nested further inside that one --
`mathlib::trig`, for instance.

`src/mathlib.scpp`:

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

Output:

```text
25
30
```

`mathlib::trig` starts with `mathlib`, so it satisfies the requirement, and
the importer reaches `sin_deg` through its full nested name,
`mathlib::trig::sin_deg`. A module is free to use nested namespaces for its
own internal organization; the rule only cares that every exported
declaration's namespace starts with the module's own name.

## A non-exported declaration can live in any namespace, or none

The namespace rule from the last two sections only constrains *exported*
declarations. A declaration that is never exported can live in any namespace
at all, unrelated to the module's own name, or in no namespace, exactly like
`square` in the very first example.

`src/mathlib.scpp`:

```cpp
export module mathlib;

namespace detail {
    int square(int x) {
        return x * x;
    }
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return detail::square(a) + detail::square(b);
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

Output:

```text
25
```

`detail` shares nothing with `mathlib`, but since `square` inside it is never
`export`-ed, that mismatch is irrelevant. `sum_of_squares` can still call
`detail::square` because that call happens inside the module's own file. An
importer cannot:

```cpp
import std;
import mathlib;

int main() {
    return detail::square(5);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'detail::square'
 5 |     return detail::square(5);
   |            ^
```

Qualifying the call with `detail::` does not help -- `main.scpp` never brought
`detail::square` into scope in the first place, because it was never exported.

## A plain `import` is private and non-transitive

So far, every importer has talked directly to the module that declares what it
needs. Real projects chain modules: one module imports another to build on it,
then exposes some of its own functionality to a third file. What that third
file can see depends entirely on how the middle module imported its own
dependency.

Add a second module, `stats`, that imports `mathlib` the ordinary way and uses
it internally:

`src/mathlib.scpp` (back to its simple form from the first section):

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/stats.scpp`:

```cpp
export module stats;

import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`, importing only `stats`:

```cpp
import std;
import stats;

int main() {
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

Output:

```text
50
```

This works: `main.scpp` never mentions `mathlib` at all, and
`sum_of_squares_twice` is `export`-ed correctly inside `namespace stats`. But
`stats.scpp`'s own `import mathlib;` is a plain import, and a plain import is
private to the file that wrote it. It makes `mathlib`'s exports visible inside
`stats.scpp`, and nowhere else:

```cpp
import std;
import stats;

int main() {
    return mathlib::sum_of_squares(3, 4);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::sum_of_squares'
 5 |     return mathlib::sum_of_squares(3, 4);
   |            ^
```

`main.scpp` never imported `mathlib`, and `stats`'s own plain `import
mathlib;` does not forward it. Only `stats`'s own exported names --
`sum_of_squares_twice`, in this case -- cross into `main.scpp`.

## `export import` re-exports transitively

Writing `export import` instead of a plain `import` changes that. It
re-exports everything the imported module exports, transitively, to whoever
imports the current module in turn.

`src/stats.scpp`, changed to re-export:

```cpp
export module stats;

export import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`, still importing only `stats`:

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

Output:

```text
25
50
```

The key change is inside `stats.scpp`: `import mathlib;` became `export
import mathlib;`. `main.scpp`'s own import list is unchanged -- still just
`import stats;` -- but it can now also call `mathlib::sum_of_squares`
directly, under `mathlib`'s own name. Re-exporting does not rename or wrap
what it forwards; it only extends who can reach it.

## The privacy and visibility rules so far

- a declaration is private to its own module unless something exports it;
- `export` only takes effect on a declaration inside a namespace matching the
  module's own name -- nested further in is fine, anywhere else is rejected;
- a declaration that is never exported can live in any namespace, or none,
  since the namespace rule does not apply to it;
- a plain `import name;` is private to the file that wrote it: it brings
  `name`'s exports into scope there, but does not forward them to whoever
  imports that file in turn;
- `export import name;` re-exports `name`'s own exports transitively, under
  their own original qualified names, to every further importer.

Every name reached in this section was a single dotted or `::`-qualified path,
written out in full each time. The next section looks more closely at those
paths themselves: how a module's own dotted name maps onto its namespace tree,
and the rules for referring to one item's exports from another location in it.

---

[← Previous: Packages and Project Manifests](ch07-01-packages-and-project-manifests.md) · [Table of Contents](README.md)
