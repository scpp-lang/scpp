# 0. Design Philosophy (the immovable North Star)

1. **It looks like C++**: anyone familiar with modern C++ should, at a glance,
   believe this is C++.
2. **Minimal additions, and they erase cleanly**: introduce new syntax only
   when strictly necessary, and only if deleting it leaves an ordinary file
   a real C++ compiler still accepts unmodified. The core addition is
   just `unsafe`: strip the keyword out (keeping its braces' contents) and
   what remains is standard C++ -- **scpp = C++ + a checker**,
   not a language with its own grammar. This is why lifetime grouping
   reuses attribute syntax rather than inventing `'a`-style tokens (see
   [§5.3](ch05-static-checks.md)), and why scpp rejected a Rust-`?`-style
   error-propagation operator in favor of ordinary `if`/`else` (see
   [§5.6](ch05-static-checks.md)/[§8](ch08-open-questions.md) Q8): a real
   C++ compiler can silently ignore an unknown attribute, but it cannot
   parse past a brand-new operator token at all.
3. **Reuse known syntax, reassign semantics**: existing spellings such as
   `std::move()`, `T&`, `unique_ptr`, `span` are given stronger *static* meaning
   (ownership / borrowing / lifetimes) without changing
   their outward appearance to the user.
4. **Safety is the default; `unsafe` is the only opt-out, and it's local
   and composable**: every function is checked
   ([§5](ch05-static-checks.md)) unconditionally, with no per-function or
   per-file annotation needed to enable it. `unsafe { }` is a lexically
   scoped block, nestable and locally reversible, that relaxes exactly
   the fixed, enumerated set of operations in
   [§5.5](ch05-static-checks.md) -- never a switch that turns off
   checking for an entire function or file. This is a deliberate reversal
   of an earlier design (a `safe` keyword marked which functions opted
   *into* checking, with unmarked functions checked not at all) -- see
   [§8](ch08-open-questions.md) Q13 for the reasoning.
5. **Soundness over compatibility**: we would rather
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
8. **No UB, ever, for anything the compiler itself controls -- not even
   inside `unsafe`.** Developers must always be able to know their
   code's exact behavior. Real C++ leaves some operations undefined even
   when a program never does anything as drastic as dereferencing a
   wild pointer -- signed integer overflow being the clearest example
   (see [§5.8](ch05-static-checks.md)): the compiler is *licensed* to
   assume it never happens and to miscompile code that (unknowingly)
   relies on it, purely because the standard says so, not because
   there's no way to define it. scpp never grants itself that license:
   for any operation whose definedness is scpp's own codegen choice
   (arithmetic overflow being the first concrete case), it either keeps
   checking it unconditionally (by default), or, inside `unsafe`,
   pins it to one specific, deterministic, documented outcome instead
   (e.g. a guaranteed two's-complement wraparound) -- never real UB.
   This doesn't extend to (and can't eliminate) the other kind of UB,
   the one that comes from an unverifiable *external* precondition
   (dereferencing a raw pointer is only ever sound if the pointer is
   genuinely valid, which no static analysis can prove in general) --
   `unsafe` still means exactly what it means in Rust for that kind.

---

[Table of Contents](README.md) · [Next: Safety Context →](ch01-safety-context.md)
