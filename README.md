# scpp

A compiler frontend for a language that **looks exactly like idiomatic modern
C++**, adding only a very small set of extensions — the core being the `safe`
keyword. Regions annotated with `safe` enable **Rust-style sound compile-time
safety checks** (ownership, borrowing, lifetimes); all other code follows
ordinary C++ semantics. The backend generates native binaries via **LLVM**.

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是 `safe`
> 关键字）。被 `safe` 标注的区域启用 Rust 式健全的编译期安全检查；其余代码按
> 普通 C++ 语义处理。后端经 LLVM 生成本地二进制。

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

The language specification is maintained in both languages:

- English: [`docs/language-spec-v0.1.en.md`](docs/language-spec-v0.1.en.md)
- 中文: [`docs/language-spec-v0.1.zh.md`](docs/language-spec-v0.1.zh.md)

## Status

Early design stage. Current milestone: **M0 — freezing the spec** before
building the M1 minimal end-to-end pipeline (scalars + locals + control flow +
functions → AST → LLVM IR → executable). See the milestones section of the spec
for the full roadmap.

## License

Released into the public domain under [The Unlicense](LICENSE).
