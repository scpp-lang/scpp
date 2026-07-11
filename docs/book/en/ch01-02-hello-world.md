# Hello, World!

Now that you have a compiler, let's make it do something visible.

Create a file named `hello.scpp`:

```cpp
import std;

int main() {
    std::println("Hello, world!");
    return 0;
}
```

If you installed `scpp` onto your `PATH` in the previous section, build it like
this:

```sh
scpp hello.scpp
./a.out
```

If you are still working directly from the repository checkout, use
`./build/scpp hello.scpp` instead.

Output:

```text
Hello, world!
```

`std::println` comes from scpp's standard library and prints one complete line
of text.

There are already a few important ideas hiding in this tiny program:

- `int main()` is the program's entry point.
- `import std;` makes the standard library available to the file.
- The program still looks like ordinary C++ on the surface, which is one of
  scpp's central design goals.

In the next section, we will keep the program just as small, but place it in a
real manifest-based project.

---

[← Previous: Building the Compiler](ch01-01-building-the-compiler.md) · [Table of Contents](README.md) · [Next: Hello, Project Builds →](ch01-03-hello-project-builds.md)
