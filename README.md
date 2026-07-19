# scpp

A compiler frontend for a language that **looks exactly like idiomatic modern
C++**, adding only a very small set of extensions -- all of them spelled as
attributes in the `scpp` namespace, so scpp introduces **zero new keywords**.
The core one is `[[scpp::unsafe]]`. Every function is checked by default with
**Rust-style sound compile-time safety checks** (ownership, borrowing,
lifetimes); `[[scpp::unsafe]] { }` blocks locally relax a fixed, enumerated
set of operations the checker can't otherwise verify. The backend generates
native binaries via **LLVM**.

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展——全都拼写成
> `scpp` 命名空间下的 attribute，所以 scpp **零新增关键字**，核心是
> `[[scpp::unsafe]]`。每个函数默认都启用 Rust 式健全的编译期安全检查（所有权、
> 借用、生命周期）；`[[scpp::unsafe]] { }` 语句块局部放宽检查器本身无法验证
> 的一小撮固定操作。后端经 LLVM 生成本地二进制。
>
> 中文版 README: [`README.zh.md`](README.zh.md)

## Design Philosophy

- **It looks like C++.** Anyone familiar with modern C++ should, at a glance,
  believe this is C++.
- **Minimal additions, zero new keywords.** New syntax only when strictly
  necessary, and always spelled as a `scpp`-namespaced attribute. The core
  addition is just `[[scpp::unsafe]]`.
- **Reuse known syntax, reassign semantics.** Existing spellings like
  `std::move()`, `T&`, `unique_ptr`, `span` gain stronger *static* meaning
  unconditionally, everywhere, without changing their outward appearance.
- **Safety is the default; `[[scpp::unsafe]]` is the only opt-out, and it's
  local and composable.** `[[scpp::unsafe]] { }` blocks relax a fixed,
  enumerated set of operations — never a switch that turns off checking for
  an entire function or file.
- **Soundness over compatibility.** We would rather
  report "not yet supported" than admit an unsound check. 100% C++ compatibility
  is a non-goal.

## Example

```cpp
int sum(std::span<const int> v) {   // checked by default: ownership, borrowing, lifetimes
    int acc = 0;
    int i = 0;
    while (i < v.size) {
        acc += v[i];   // bounds-checked by default
        i = i + 1;
    }
    return acc;
}

int legacy(int* p) {
    [[scpp::unsafe]] {
        return *p;   // raw pointer dereference needs an explicit [[scpp::unsafe]] block
    }
}
```

## Documentation

The full language specification is "The SCPP Programming Language" book. The
book currently has three maintained editions:

- English: [`docs/book/en/README.md`](docs/book/en/README.md)
- 简体中文: [`docs/book/zh/README.md`](docs/book/zh/README.md)
- 繁體中文（台灣）: [`docs/book/zh-TW/README.md`](docs/book/zh-TW/README.md)

Other documentation trees, such as the spec and design documents, are still
published only in English and Simplified Chinese today.

## Building

Requires Clang with C++23 modules support, CMake 3.28+, Ninja, an LLVM
development package, SQLite development headers/libs, and a `g++` install (for
its `libstdc++` development files -- see below). On Debian/Ubuntu:

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libzstd-dev libsqlite3-dev g++
```

(`libzstd-dev` is needed because LLVM's CMake config links against it;
without it, `find_package(LLVM)` fails with a missing `zstd::libzstd_shared`
target. `libsqlite3-dev` provides the `sqlite3.h` header and static/shared
SQLite library that SCPP now links during full source builds. `g++` is not
used as the compiler -- that's still Clang -- but its `libstdc++-*-dev`
package provides the `bits/std.cc` module interface unit Clang's `import
std;` builds against; SCPP's own C++ implementation uses `import std;`
everywhere instead of `#include`-ing standard library headers, and
deliberately targets libstdc++ rather than libc++ so it stays ABI-compatible
with the prebuilt LLVM libraries above, which are themselves built against
libstdc++.)

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<path to LLVM's lib/cmake/llvm>
cmake --build build
ctest --test-dir build
```

To install a built scpp from source, use CMake's ordinary prefix-based install
step:

```sh
cmake --install build --prefix <your-chosen-directory>
```

This produces a self-contained tree under that prefix:

```text
<prefix>/
├── bin/scpp
└── share/scpp/stdlib/
```

Then add `<prefix>/bin` to your `PATH` however you prefer. scpp itself does not
modify shell rc files or manage installed toolchains.

The `scpp` CLI supports:

```sh
scpp lex <file>              # dump the token stream
scpp parse <file>            # dump the AST
scpp build <file> [-o <out>] # compile to a native executable via LLVM
```

## License

Released into the public domain under [The Unlicense](LICENSE).
