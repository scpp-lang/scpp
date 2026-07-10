# Hello, Project Builds

Single-file compilation is great for quick experiments. The moment you want a
named binary and a project directory, scpp's manifest-based build mode is more
comfortable.

Create a directory and put these two files in it.

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "starter"
version = "0.1.0"

[[bin]]
name = "hello"
root = "main.scpp"
sources = ["*.scpp"]
```

`main.scpp`:

```cpp
extern "C" int puts(const char* s);

int main() {
    [[scpp::unsafe]] {
        puts("Hello from a project build!");
    }
    return 0;
}
```

From that directory, build the project:

```sh
scpp build
./.scpp/build/*/dev/starter/hello
```

Output:

```text
Hello from a project build!
```

The `*` in the output path expands to your target triple, such as
`x86_64-pc-linux-gnu`. scpp places build artifacts under `.scpp/build/`, so the
project directory itself stays small and predictable.

This is enough for a first chapter:

- you built the compiler;
- you compiled a one-file program;
- you built a manifest-based project with a named binary.

The next chapter will stay hands-on, but switch from setup to a slightly larger
program that uses ordinary variables, loops, and conditions together.

---

[← Previous: Hello, World!](ch01-02-hello-world.md) · [Table of Contents](README.md)
