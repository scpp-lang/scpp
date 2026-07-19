# Using `import` and Qualified Names

Every example since modules were introduced has paired the same two things by
hand: an `import` line naming a module, and a full `::`-qualified path
written out at each call site to reach anything inside it. This section
studies `import` itself in isolation -- exactly what forms it accepts, what
it does and does not put in scope, and how it and a path's own qualified
name actually work together.

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

## `import` names one whole module -- never a single item inside it

Every `import` so far has had the same shape: the keyword, one dotted name,
a semicolon. Since a path already uses `::` to reach one specific item inside
a module, it is natural to wonder whether `import` accepts the same thing --
importing just `sum_of_squares` out of `mathlib`, say, instead of the whole
module.

`src/mathlib.scpp`:

```cpp
export module mathlib;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib::sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

Compiler output:

```text
src/main.scpp:2:15: error: expected ';' but found '::'
 2 | import mathlib::sum_of_squares;
   |               ^
```

`::` never appears inside an `import` line at all -- only afterward, in a
path, at the call site. Writing a dot instead, matching the way a module's
own multi-segment name is spelled, at least parses:

```cpp
import std;
import mathlib.sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

Compiler output:

```text
src/main.scpp: error: cannot find module 'mathlib.sum_of_squares' (use --import mathlib.sum_of_squares=path/to/file or -I <dir>)
```

This fails for a different reason. [Paths for Referring to Items in the
Module Tree](ch07-03-paths-for-referring-to-items-in-module-tree.md) showed
a dot joining a module's own name into segments, as in `mathlib.trig`. The
same rule applies here: `mathlib.sum_of_squares` is read as the name of a
module with two segments, `mathlib` and `sum_of_squares` -- not as "the item
`sum_of_squares` inside the module `mathlib`" -- and no module by that name
exists. A dot inside an `import` line is always another module-name segment,
never an item selector. There is no partial form of `import`: every `import`
names one whole module, in full, or it names nothing at all.

## Importing a module does not bring its names into scope unqualified

Given that `import mathlib;` is the only way to depend on `mathlib` at all,
what does it actually put in scope? Every example back through chapter 1
already answers this, without ever calling it out: `import std;` has never
once let a later line call `println` on its own -- it was always
`std::println`. The same is true of any other module.

```cpp
import std;
import mathlib;

int main() {
    return sum_of_squares(3, 4);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'sum_of_squares'
 5 |     return sum_of_squares(3, 4);
   |            ^
```

`mathlib` really is imported, and `sum_of_squares` really is exported from
it, but the bare name `sum_of_squares` is exactly as unknown here as an
identifier that was never declared anywhere. Only the qualified form reaches
it:

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

Dropping `std::` in front of `println` fails the exact same way, for the
exact same reason -- `std` is a module like any other, brought into scope by
`import` exactly like `mathlib`:

```cpp
import std;

int main() {
    println("{}", 42);
    return 0;
}
```

Compiler output:

```text
src/main.scpp:4:5: error: call to unknown function 'println'
 4 |     println("{}", 42);
   |     ^
```

`import` only makes a module's exports reachable through their own full
path. It never shortens that path, and it never drops any of a module's
names into scope on their own -- not even the standard library's.

## The same rule covers every exported item, not only functions

Every name reached this way so far has been a function, but the rule is not
function-specific. It applies just as much to a `struct`.

`src/mathlib.scpp`, with a `Point` added alongside `sum_of_squares`:

```cpp
export module mathlib;

namespace mathlib {
    export struct Point {
        int x;
        int y;
    };

    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    mathlib::Point p{};
    p.x = 3;
    p.y = 4;
    std::println("{}", mathlib::sum_of_squares(p.x, p.y));
    return 0;
}
```

Output:

```text
25
```

`Point` is constructed under `mathlib::Point`, its own full qualified name,
exactly the way `sum_of_squares` is called under `mathlib::sum_of_squares`.
A bare `Point` is no more in scope here than a bare `sum_of_squares` was in
the section above -- `import` put both of them in scope the same way, which
is to say: only under their own full path, regardless of what kind of item
each one is.

## Every `import` must come before anything else in the file

Every `import` line in every example so far, in this section and the two
before it, has appeared at the very top of its file, ahead of every other
declaration. That is not a style choice.

```cpp
import std;

int triple(int x) {
    return x * 3;
}

import std;

int main() {
    std::println("{}", triple(4));
    return 0;
}
```

Compiler output:

```text
src/main.scpp:7:1: error: expected a type name
 7 | import std;
   | ^
```

The second `import std;` is rejected -- not because importing `std` twice is
itself a problem, but because of where it sits. Once parsing moves past the
initial run of `import` and `export import` lines at the top of a file,
`import` is no longer recognized as the start of anything at all. Every
`import` a file needs has to be grouped together, before every other
declaration in it.

## A plain `import` and `export import` still only decide who can reach a name

[Control Scope and Privacy with Modules](ch07-02-control-scope-and-privacy-with-modules.md)
already covered the difference between the two: a plain `import name;` is
private to the file that wrote it, while `export import name;` re-exports
`name`'s own exports transitively, under their own original names, to
whoever imports the current module in turn. That distinction still holds
exactly as described there -- what is new here is checking it against a
module whose own name has more than one segment.

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/stats.scpp`, importing it the ordinary, private way:

```cpp
export module stats;

import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`, importing only `stats`:

```cpp
import std;
import stats;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::sin_deg'
 5 |     return mathlib::trig::sin_deg(30);
   |            ^
```

Exactly as with a single-segment module name, `stats.scpp`'s own plain
`import mathlib.trig;` does not forward `mathlib::trig::sin_deg` to anyone
who imports `stats` in turn. Changing that one line to `export import
mathlib.trig;` changes the outcome:

`src/stats.scpp`:

```cpp
export module stats;

export import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`, still importing only `stats`:

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    std::println("{}", stats::double_sin_deg(30));
    return 0;
}
```

Output:

```text
30
60
```

`mathlib::trig::sin_deg` reaches `main.scpp` under exactly the same two
segments it already had -- re-exporting a multi-segment module changes
nothing about how many segments its path needs, or what any of them are
called. A plain `import` and `export import` only ever decide which files
can follow a path; neither one ever changes the path itself.

## There is no aliasing for an `import`

C++ can bind a long qualified name to a shorter one with `namespace alias =
long::qualified::name;`. scpp's `import` has nothing like it.

```cpp
import std;
import mathlib as m;

int main() {
    return m::sum_of_squares(3, 4);
}
```

Compiler output:

```text
src/main.scpp:2:16: error: expected ';' but found 'as'
 2 | import mathlib as m;
   |                ^
```

`import` has exactly one form: the keyword, one dotted module name, a
semicolon -- optionally preceded by `export`. Nothing renames a module on
the way in, and nothing shortens the path once it is imported. Every call
site in this section, and the two sections before it, spells out the same
path its module's own name and namespace already produce, in full, every
time.

## The import and qualified-name rules so far

- an `import` always names one whole module, in full -- there is no way to
  name just one item inside it, and `::` never appears inside the `import`
  line itself, only afterward, in a path;
- importing a module never brings any of its names into scope unqualified,
  for any kind of item -- reaching them always takes their own full path;
- every `import` in a file must come before every other declaration in it;
- a plain `import` only makes a name reachable inside the file that wrote
  it; `export import` still forwards that same name, with the same path,
  however many segments it has;
- scpp has no aliasing for an import or a qualified name -- every call site
  writes out the one path its module already chose.

So far, one module has always meant exactly one file. The next section
looks at what changes -- and what does not -- once a module's own source
needs to spread across more than one.

---

[← Previous: Paths for Referring to Items in the Module Tree](ch07-03-paths-for-referring-to-items-in-module-tree.md) · [Table of Contents](README.md)
