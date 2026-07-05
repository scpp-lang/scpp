# scpp

A compiler frontend for a language that **looks exactly like idiomatic modern
C++**, adding only a very small set of extensions — the core being the `unsafe`
keyword. Every function is checked by default with **Rust-style sound
compile-time safety checks** (ownership, borrowing, lifetimes); `unsafe { }`
blocks locally relax a fixed, enumerated set of operations the checker can't
otherwise verify. The backend generates native binaries via **LLVM**.

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是 `unsafe`
> 关键字）。每个函数默认都启用 Rust 式健全的编译期安全检查（所有权、借用、
> 生命周期）；`unsafe { }` 语句块局部放宽检查器本身无法验证的一小撮固定
> 操作。后端经 LLVM 生成本地二进制。
>
> 中文版 README: [`README.zh.md`](README.zh.md)

## Design Philosophy

- **It looks like C++.** Anyone familiar with modern C++ should, at a glance,
  believe this is C++.
- **Minimal additions.** New syntax only when strictly necessary. The core
  addition is just `unsafe`.
- **Reuse known syntax, reassign semantics.** Existing spellings like
  `std::move()`, `T&`, `unique_ptr`, `span` gain stronger *static* meaning
  unconditionally, everywhere, without changing their outward appearance.
- **Safety is the default; `unsafe` is the only opt-out, and it's local and
  composable.** `unsafe { }` blocks relax a fixed, enumerated set of
  operations — never a switch that turns off checking for an entire function
  or file.
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
    unsafe {
        return *p;   // raw pointer dereference needs an explicit unsafe { } block
    }
}
```

## Documentation

The full language specification is "The SCPP Programming Language" book,
maintained in both languages:

- English: [`docs/book/en/README.md`](docs/book/en/README.md)
- 中文: [`docs/book/zh/README.md`](docs/book/zh/README.md)

## Building

Requires Clang with C++23 modules support, CMake 3.28+, Ninja, and an LLVM
development package. On Debian/Ubuntu:

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libzstd-dev
```

(`libzstd-dev` is needed because LLVM's CMake config links against it;
without it, `find_package(LLVM)` fails with a missing `zstd::libzstd_shared`
target.)

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<path to LLVM's lib/cmake/llvm>
cmake --build build
ctest --test-dir build
```

The `scpp` CLI supports:

```sh
scpp lex <file>              # dump the token stream
scpp parse <file>            # dump the AST
scpp build <file> [-o <out>] # compile to a native executable via LLVM
```

## License

Released into the public domain under [The Unlicense](LICENSE).
