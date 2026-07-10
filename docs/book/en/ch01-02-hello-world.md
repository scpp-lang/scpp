# Hello, World!

Now that you have a compiler, let's make it do something visible.

Create a file named `hello.scpp`:

```cpp
extern "C" int puts(const char* s);

int main() {
    [[scpp::unsafe]] {
        puts("Hello, world!");
    }
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

There are already a few important ideas hiding in this tiny program:

- `int main()` is the program's entry point.
- `extern "C"` lets scpp declare a function supplied by the C runtime.
- `puts` is not checked by the scpp compiler, so calling it requires an
  explicit `[[scpp::unsafe]]` block.
- The program still looks like ordinary C++ on the surface, which is one of
  scpp's central design goals.

Do not worry if `[[scpp::unsafe]]` feels mysterious right now. For the moment,
you can read it as: “this is the exact local place where I am trusting
something outside the compiler's proof.”

In the next section, we will keep the program just as small, but place it in a
real manifest-based project.

---

[← Previous: Building the Compiler](ch01-01-building-the-compiler.md) · [Table of Contents](README.md) · [Next: Hello, Project Builds →](ch01-03-hello-project-builds.md)
