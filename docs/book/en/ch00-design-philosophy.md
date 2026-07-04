# 0. Design Philosophy (the immovable North Star)

1. **It looks like C++**: anyone familiar with modern C++ should, at a glance,
   believe this is C++.
2. **Minimal additions, and they erase cleanly**: introduce new syntax only
   when strictly necessary, and only if deleting it leaves an ordinary file
   a real C++ compiler still accepts unmodified. The core additions are
   just `safe` / `unsafe`: strip both keywords out (keeping `unsafe`'s
   braces) and what remains is standard C++ -- **scpp = C++ + a checker**,
   not a language with its own grammar. This is why lifetime grouping
   reuses attribute syntax rather than inventing `'a`-style tokens (see
   [§5.3](ch05-static-checks.md)), and why scpp rejected a Rust-`?`-style
   error-propagation operator in favor of ordinary `if`/`else` (see
   [§5.6](ch05-static-checks.md)/[§8](ch08-open-questions.md) Q8): a real
   C++ compiler can silently ignore an unknown attribute, but it cannot
   parse past a brand-new operator token at all.
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
6. **Erasure beats ergonomics and completeness**: no other attempt at
   bringing Rust-level soundness to a C-family language has managed to
   keep the erasure property (§2) *while also* getting there -- tools
   that stay erasable (e.g. Microsoft's SAL annotations) end up far
   weaker than Rust; tools that reach real soundness (Sean Baxter's
   Circle/Safe C++) require their own compiler and abandon erasure
   entirely; efforts occupying the middle (Checked C) had to give up
   erasure for exactly the constructs (pointer types, bounds
   annotations) that needed new grammar to express it. Doing *both at
   once* is scpp's actual bet and its differentiator. So when a future
   feature would need genuinely new, non-erasable grammar to be fully
   expressed or to feel as ergonomic as Rust's equivalent, the answer is
   **not** to add the grammar: find a (possibly more verbose) way to say
   the same thing with syntax that already erases cleanly, or leave the
   feature unsupported until one is found (see [§8](ch08-open-questions.md)
   Q8 for the `??` operator as the first concrete instance of this call).
   Verbosity and missing features are an acceptable, recoverable cost; a
   broken erasure guarantee is not.
7. **Reference C++ standard: C++26.** Wherever this document calls a name
   or feature "real"/"already-standardized" C++ (e.g. `<cstdint>`,
   `<stdfloat>`, `std::expected`, C++20 modules) versus "not yet
   standard" (e.g. `int128_t`/WG21 P1467 -- see [§6](ch06-safe-subset.md)),
   the cutoff is **C++26 specifically**: feature-frozen since the June
   2025 Sofia meeting and through its final ballot as of the March 2026
   London meeting. This is the baseline Principle 2's erasure property
   and Principle 6's "stick to ratified names" rule are checked against
   -- bump it forward whenever a newer standard is ratified.

---

[Table of Contents](README.md) · [Next: Safety Context →](ch01-safety-context.md)
