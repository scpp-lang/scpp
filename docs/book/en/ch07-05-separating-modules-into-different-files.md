# Separating Modules into Different Files

So far, one module has always meant exactly one file. Real programs rarely
stay that small. When one module's own source needs to spread across several
files, scpp keeps one primary interface unit and lets the rest of that same
module attach to it as partitions.

Today, in ordinary `scpp build` project builds, this is the supported way to
split one module across files: a primary interface unit plus one or more
partitions.

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

## `export import :part;` re-exports an interface partition

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :trig;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/trig.scpp`:

```cpp
export module mathlib:trig;

namespace mathlib {
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
    std::println("{}", mathlib::sin_deg(30));
    return 0;
}
```

Output:

```text
25
30
```

`mathlib.scpp` is still the module's primary interface unit: it is the one
file that says `export module mathlib;`. `trig.scpp` belongs to that same
module, but names one partition of it with `export module mathlib:trig;`.
The primary interface unit re-exports that partition with `export import
:trig;`, so a file outside the module still writes only `import mathlib;`.
Nothing outside the module imports `:trig` directly.

## A partition name is not another `::` segment in a path

The partition above was named `:trig`, but that does not make `trig` part of
any exported item's own path.

`src/main.scpp`:

```cpp
import mathlib;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

Compiler output:

```text
src/main.scpp:4:12: error: call to unknown function 'mathlib::trig::sin_deg'
 4 |     return mathlib::trig::sin_deg(30);
   |            ^
```

`sin_deg` is exported from `namespace mathlib`, so its path is
`mathlib::sin_deg`. The partition name `trig` helps organize the module's own
source files, but it does not create a new namespace segment and it does not
change any qualified name. As in the previous section, a path still comes only
from the declaration's namespace nesting.

## `import :part;` keeps a partition internal to the module

A partition can also be imported for internal use only.

`src/mathlib.scpp`:

```cpp
export module mathlib;

import :detail;

namespace mathlib {
    export int doubled_sum(int a, int b) {
        return double_it(a + b);
    }
}
```

`src/detail.scpp`:

```cpp
module mathlib:detail;

namespace mathlib {
    int double_it(int x) {
        return x * 2;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::doubled_sum(3, 4));
    return 0;
}
```

Output:

```text
14
```

Here `detail.scpp` is an implementation partition: `module mathlib:detail;`
with no `export` on the module declaration itself. The primary interface unit
uses a plain `import :detail;` to reach it internally, and that is enough for
`doubled_sum` to call `double_it`.

An importer still cannot reach `double_it` directly:

```cpp
import std;
import mathlib;

int main() {
    return mathlib::double_it(5);
}
```

Compiler output:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::double_it'
 5 |     return mathlib::double_it(5);
   |            ^
```

A plain `import :detail;` only wires another file into the same module's own
implementation. It does not make that partition part of the module's public
surface.

## Only the module's own files can import a partition

A file outside the module still cannot name a partition after `import`.

`src/main.scpp`:

```cpp
import mathlib:trig;

int main() {
    return 0;
}
```

Compiler output:

```text
src/main.scpp:1:15: error: expected ';' but found ':'
 1 | import mathlib:trig;
   |               ^
```

Outside the module, `import` still names one whole module, exactly as the
previous section established. The `:trig` spelling is only for files that
already belong to `mathlib` and are importing another part of that same
module. External code imports the whole module and gets whatever the primary
interface unit chose to re-export.

## `export import` only works on an interface partition

Because an implementation partition is internal by construction, trying to
re-export one is rejected.

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :detail;
```

Compiler output:

```text
src/mathlib.scpp:3:8: error: cannot 'export import' partition 'mathlib:detail': it is an implementation partition ('module ...;' with no 'export' on its own module declaration), so it can never export anything to the outside (ch11 §11.4)
 3 | export import :detail;
   |        ^
```

Re-exporting is only for interface partitions -- files that declared
`export module name:part;`. An implementation partition can contribute code
used inside the module, but it can never become part of the module's exported
surface.

## One module, one primary interface, many files

- one file still declares `export module mathlib;` -- the primary interface
  unit;
- extra files join that same module as partitions named `module
  mathlib:part;` or `export module mathlib:part;`;
- external files still write `import mathlib;`, never a partition name;
- partition names organize source files, not qualified paths;
- `export import :part;` re-exports an interface partition, while plain
  `import :part;` keeps a partition internal.

With partitions, a module can grow across several files without reopening
anything the previous sections already established: exports still decide
visibility, namespaces still decide paths, and `import` still brings in whole
modules rather than individual items.

---

[← Previous: Using `import` and Qualified Names](ch07-04-using-import-and-qualified-names.md) · [Table of Contents](README.md)
