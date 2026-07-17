# Packages and Project Manifests

Every scpp program so far has lived in one file, or in the small
manifest-based project you already built in
[Hello, Project Builds](ch01-03-hello-project-builds.md). This chapter goes
further: how a real scpp project is laid out, how one project can produce
more than one binary, and how those binaries share code with each other.

Two words matter for the rest of this chapter, and they are not the same
thing:

- a **package** is a directory with a `scpp.toml` manifest at its root. It is
  the unit `scpp build` operates on.
- a **module** is the compilation unit introduced by `export module` /
  `module` and consumed with `import`. It is the unit the compiler itself
  reasons about when checking what is visible from where.

A package's manifest can declare any number of binaries and any number of
libraries. Modules are how those libraries and those binaries actually share
declarations. This section stays at the package level; the next section moves
down into modules themselves.

For each project below, create the files shown, then from inside that
project's own directory run:

```sh
scpp build
```

Binaries land under `.scpp/build/<target triple>/dev/<package name>/<binary
name>`, exactly as in
[Hello, Project Builds](ch01-03-hello-project-builds.md).

## A package needs a manifest and at least one target

`manifest-version = 1` plus a `[package]` table with `name` and `version` are
not enough by themselves. A manifest also needs at least one build target: a
`[[bin]]` table, a `[[lib]]` table, or both.

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"
```

```sh
scpp build
```

Compiler output:

```text
error: manifest must declare at least one [[lib]] or [[bin]] target
```

The rest of this section adds targets to this same manifest, one at a time.

## `sources` is a glob pattern you write, not a fixed filename

A `[[bin]]` table needs a `name` and a `sources` list. `name` becomes both
the binary's own file name and the identifier `--bin` selects later.
`sources` is a list of glob patterns, expanded against the package's own
directory tree -- `*` matches within one directory, `**` matches across
directories. Nothing about scpp reserves a fixed file name for a binary's
root; you choose the layout yourself through `sources`.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["*.scpp"]
```

`greeter.scpp`:

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
```

Output:

```text
Hello, scpp!
```

Here `sources = ["*.scpp"]` matches every `.scpp` file directly inside the
package root -- there is no `src/` directory yet, and none is required.

## One package can build more than one binary

A manifest can declare any number of `[[bin]]` tables. Each is compiled and
linked from whatever sources it names, independently of the others. As a
project grows past one file, moving sources under `src/` keeps each target's
glob pattern pointed at exactly the files it owns.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greeter.scpp`:

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

`src/shout.scpp`:

```cpp
import std;

int main() {
    std::println("HELLO, SCPP!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

Output:

```text
Hello, scpp!
HELLO, SCPP!
```

Plain `scpp build` builds both binaries. To build only one of them, name it
explicitly:

```sh
scpp build --bin shout
./.scpp/build/*/dev/greeter/shout
```

Output:

```text
HELLO, SCPP!
```

`[[bin]]` names must also be unique within a package. Two `[[bin]]` tables
both named `"greeter"` are rejected before either one is compiled:

```text
error: duplicate [[bin]] target name 'greeter'
```

## A library target lets binaries share code automatically

A `[[lib]]` table works like `[[bin]]` -- a `name` and a `sources` list --
but it compiles a module instead of linking an executable. Inside that
module's source, `export module greetings;` names the module, and a matching
`namespace greetings { ... }` marks which declarations `export` makes visible
to importers.

Every `[[bin]]` target in the same package can `import` that module by name,
with no extra flags: `scpp build` already compiled it earlier in the same
run, and the manifest tells the rest of the build where its interface and
archive live.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greetings.scpp`:

```cpp
export module greetings;

namespace greetings {
    export const char* phrase(bool shout) {
        return shout ? "HELLO, SCPP!" : "Hello, scpp!";
    }
}
```

`src/greeter.scpp`:

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(false));
    return 0;
}
```

`src/shout.scpp`:

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

Output:

```text
Hello, scpp!
HELLO, SCPP!
```

Same output as before, but now `greeter` and `shout` share one implementation
instead of repeating the phrase in each file. Building also leaves the
library's own compiled artifacts alongside the two binaries:

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
```

`--lib` builds just the library, without linking either binary:

```sh
scpp build --lib
```

## One package can build more than one library

A manifest can declare any number of `[[lib]]` tables, exactly as it can any
number of `[[bin]]` tables. Every `[[bin]]` target in the package can
`import` any of them, not only the one it happens to need -- there is
nothing special about the single `[[lib]]` table used above.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[lib]]
name = "farewells"
sources = ["src/farewells.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

This adds a second library, `farewells`, alongside the existing `greetings`.

`src/farewells.scpp`:

```cpp
export module farewells;

namespace farewells {
    export const char* phrase() {
        return "Goodbye, scpp!";
    }
}
```

`greeter` now imports both libraries -- no manifest option turns this on,
`import` is all it takes.

`src/greeter.scpp`:

```cpp
import std;
import greetings;
import farewells;

int main() {
    std::println("{}", greetings::phrase(false));
    std::println("{}", farewells::phrase());
    return 0;
}
```

`shout` is unchanged, and still imports only `greetings` -- a `[[bin]]`
target is free to import any subset of the package's `[[lib]]` targets, not
necessarily all of them.

`src/shout.scpp`:

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

Output:

```text
Hello, scpp!
Goodbye, scpp!
HELLO, SCPP!
```

Building now leaves both libraries' artifacts side by side:

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/modules/farewells.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
.scpp/build/*/dev/greeter/archives/libfarewells.scppa
```

Plain `--lib` still builds every library target and links no binary, exactly
as it did with only one. To build a single one of several, name it, the same
way `--bin` selects one binary:

```sh
scpp build --lib farewells
```

This builds `farewells` alone; `greetings` and both binaries are left
untouched.

`[[lib]]` names must also be unique within a package, exactly like `[[bin]]`
names. Two `[[lib]]` tables both named `"greetings"` are rejected the same
way:

```text
error: duplicate [[lib]] target name 'greetings'
```

## The manifest rules so far

- a package is a directory with `scpp.toml` at its root;
- the manifest needs `manifest-version = 1`, a `[package]` table, and at
  least one `[[bin]]` or `[[lib]]` table;
- `sources` is a list of glob patterns you write yourself, not a fixed file
  name;
- a package can build any number of binaries, selected individually with
  `--bin <name>`;
- a package can build any number of libraries, selected individually with
  `--lib <name>`, or all of them at once with plain `--lib`;
- every `[[bin]]` target in a package automatically sees all of that
  package's `[[lib]]` modules, with no `--import` flag needed.

Packages are the build-level story. The next section moves to the language
level: how modules use namespaces to control scope and privacy inside and
across those files.

---

[← Previous: Localizing Trust in Real Programs](ch06-03-localizing-trust-in-real-programs.md) · [Table of Contents](README.md)
