# Building the Compiler

scpp is currently built from source. If you have the repository checked out,
the first job is to produce a working `scpp` binary.

## What you need

For a source build, prepare:

- CMake 3.28 or newer
- Ninja
- Clang/LLVM 22
- SQLite development headers and libraries
- zstd development headers and libraries

On Debian or Ubuntu, that looks like this:

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libsqlite3-dev libzstd-dev
```

## Configure and build

From the repository root, run:

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build
```

When that finishes, the freshly built compiler is available at:

```text
./build/scpp
```

## Optional: install it somewhere on your `PATH`

If you want the rest of this chapter's commands to use `scpp` directly instead
of `./build/scpp`, install the build into a prefix you control:

```sh
cmake --install build --prefix "$HOME/.local/scpp"
export PATH="$HOME/.local/scpp/bin:$PATH"
```

That install step creates a self-contained tree containing the compiler and the
stdlib files it needs.

If you prefer not to install anything yet, that is fine too. You can keep using
`./build/scpp` from the repository root.

## What you have now

At this point you have a real compiler binary. The next section uses it to
build the smallest possible scpp program.

---

[← Previous: Getting Started](ch01-00-getting-started.md) · [Table of Contents](README.md) · [Next: Hello, World! →](ch01-02-hello-world.md)
