# 0. Design Philosophy (the immovable North Star)

1. **It looks like C++**: anyone familiar with modern C++ should, at a glance,
   believe this is C++.
2. **Minimal additions**: introduce new syntax only when strictly necessary.
   The core additions are just `safe` / `unsafe`.
3. **Reuse known syntax, reassign semantics**: existing spellings such as
   `std::move()`, `T&`, `unique_ptr`, `span` are given stronger *static* meaning
   inside `safe` regions (ownership / borrowing / lifetimes) without changing
   their outward appearance to the user.
4. **Safety is opt-in, local, and composable**: safety is enabled per region
   (function / block / type); unannotated code retains full C++ freedom (and
   unsafety).
5. **Soundness over compatibility**: within a `safe` region we would rather
   temporarily report a construct as "not yet supported" than admit an unsound
   check. 100% C++ compatibility is a non-goal.

---

[Table of Contents](README.md) · [Next: Safety Context →](ch01-safety-context.md)
