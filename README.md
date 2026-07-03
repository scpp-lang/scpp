# scpp

A compiler frontend for a language that **looks exactly like idiomatic modern
C++**, adding only a very small set of extensions — the core being the `safe`
keyword. Regions annotated with `safe` enable **Rust-style sound compile-time
safety checks** (ownership, borrowing, lifetimes); all other code follows
ordinary C++ semantics. The backend generates native binaries via **LLVM**.

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是 `safe`
> 关键字）。被 `safe` 标注的区域启用 Rust 式健全的编译期安全检查；其余代码按
> 普通 C++ 语义处理。后端经 LLVM 生成本地二进制。
>
> 中文版 README: [`README.zh.md`](README.zh.md)

## Design Philosophy

- **It looks like C++.** Anyone familiar with modern C++ should, at a glance,
  believe this is C++.
- **Minimal additions.** New syntax only when strictly necessary. The core
  additions are just `safe` / `unsafe`.
- **Reuse known syntax, reassign semantics.** Existing spellings like
  `std::move()`, `T&`, `unique_ptr`, `span` gain stronger *static* meaning
  inside `safe` regions without changing their outward appearance.
- **Safety is opt-in, local, and composable.** Unannotated code keeps full C++
  freedom (and unsafety).
- **Soundness over compatibility.** Inside a `safe` region we would rather
  report "not yet supported" than admit an unsound check. 100% C++ compatibility
  is a non-goal.

## Example

```cpp
safe int sum(const std::vector<int>& v) {   // checks enabled for this body
    int acc = 0;
    for (auto x : v) acc += x;
    return acc;
}

int legacy() {                               // ordinary C++, unchecked
    int* p = nullptr; return *p;             // allowed; you are on your own
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

## Status

Early design stage.

- **M1 — minimal end-to-end pipeline** (scalars + locals + control flow +
  functions → AST → LLVM IR → executable, no `safe` checks yet): done.
- **M2 — type system + `struct` + `unique_ptr` + move semantics** (trivial
  `struct`s with a fixed, Clang-ABI-compatible memory layout; `std::move` as
  a compiler-recognized move hint; move-out checking so a moved-from
  `std::unique_ptr` can't be read again): done.
- **M3 — MIR + initialization checking + drop insertion** (a CFG-based MIR;
  a 2-phase worklist dataflow analysis for move/initialization checking,
  lexically scope-aware on both the codegen side — real per-scope
  `std::unique_ptr` drop insertion — and the move-checker side): done.
- **M4 — borrow & alias-XOR-mutability checking** (intraprocedural, first
  slice): `T&` (mutable/exclusive borrow) and `const T&` (shared borrow) for
  local reference variables and function reference parameters, with
  alias-XOR-mutability enforced via a per-place borrow lattice; borrow
  duration is lexically scoped (not yet NLL-style liveness — that's M5).
  References to `std::unique_ptr`, returning a reference, and reference
  struct fields are deferred to a later version.

See the milestones chapter of [the book](docs/book/en/README.md) for the
full roadmap.

## License

Released into the public domain under [The Unlicense](LICENSE).
