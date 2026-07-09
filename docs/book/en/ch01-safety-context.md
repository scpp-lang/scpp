# A Small Complete Program

The fastest way to get comfortable with a language is to build something that
feels like a whole program, not just a disconnected fragment. In this chapter
we will write a tiny countdown program. It is still small enough to understand
in one sitting, but big enough to introduce variables, a loop, and a branch.

## The program

Create `countdown.scpp`:

```cpp
extern "C" int printf(const char* fmt, ...);
extern "C" int puts(const char* s);

int main() {
    int n = 5;

    while (n > 0) {
        if (n == 1) {
            [[scpp::unsafe]] {
                puts("Ignition!");
            }
        } else {
            [[scpp::unsafe]] {
                printf("T-minus %d\n", n);
            }
        }
        n = n - 1;
    }

    [[scpp::unsafe]] {
        puts("Liftoff!");
    }
    return 0;
}
```

Build and run it:

```sh
./build/scpp countdown.scpp
./a.out
```

Expected output:

```text
T-minus 5
T-minus 4
T-minus 3
T-minus 2
Ignition!
Liftoff!
```

## Read it top to bottom

This program introduces a few core habits:

- Start with `main`. Execution begins there.
- Store state in a variable. Here, `n` remembers the current countdown value.
- Use `while` when you want to repeat work.
- Use `if` / `else` when one step depends on the current value.
- Update the variable yourself. `n = n - 1;` makes the loop move forward.

Even though this is still a toy program, it already feels like real code:
there is state, a decision, repetition, and visible output.

## A note about the printing calls

We are still borrowing two C-library functions, `printf` and `puts`, to make
our examples visible. That is why the calls sit inside `[[scpp::unsafe]]`
blocks. This chapter is about learning the shape of a whole program, not about
fully unpacking FFI and safety boundaries yet.

## Next

The next chapter slows down and names the building blocks we just used: scalar
values, variables, functions, and control flow.

## Reference appendix preserved during the rewrite

The material below is older, reference-oriented content that later chapters
still link to. It remains here temporarily so those links keep working while
the tutorial rewrite is landing in batches.

Every function is checked ([§5](ch05-static-checks.md)) **by default,
unconditionally** -- there is no per-function or per-file annotation that
turns this on. The only safety-context construct in the language is the
`[[scpp::unsafe]]` attribute: applied to a compound-statement, it makes
that lexically scoped block a local, composable escape hatch that relaxes
exactly the fixed, enumerated set of operations in
[§5.5](ch05-static-checks.md); applied instead to a function's own
declaration, it makes calling that function one more such operation, and
makes the function's *entire* body an unsafe context throughout (see
[§1.2](#12-marking-an-entire-function-nesting-vs-the-function-level-marker)).
Either way, `[[scpp::unsafe]]` relaxes nothing else. `scpp::unsafe`
is an *attribute*, not a keyword -- scpp introduces no new keywords at all
(see [ch00](ch00-design-philosophy.md) §2). This is a deliberate
reversal of an earlier design (a `safe` keyword that opted individual
functions *into* checking, with unmarked functions checked not at all) --
see [§8](ch08-open-questions.md) Q13 for the full reasoning behind the
reversal; that reversal is unaffected by, and orthogonal to, the
function-level marker described below -- **every** function, marked or
not, still gets full [§5.1-§5.4](ch05-static-checks.md) checking,
unconditionally.

## 1.1 What `[[scpp::unsafe]]` relaxes, and what stays unconditional

- **Everything in [§5.1-§5.4](ch05-static-checks.md)** -- ownership/move
  state, alias-XOR-mutability, lifetime/dangling checks, and zero-init --
  runs **unconditionally, everywhere, including inside an
  `[[scpp::unsafe]]` context**. `[[scpp::unsafe]]` never disables this
  analysis; there is no construct anywhere in scpp that does. This matches
  Rust exactly: an `unsafe fn`'s body, or the inside of an `unsafe { }`
  block, is still fully borrow-checked in Rust -- `unsafe` only unlocks a
  handful of specific operations the borrow checker itself cannot verify,
  it never suspends borrow checking as a whole.
- **Only the operations [§5.5](ch05-static-checks.md) lists become legal
  inside an `[[scpp::unsafe]]` context**: raw pointer dereference/arithmetic,
  `reinterpret_cast`/incompatible C-style casts, untagged `union` member
  access, raw `new`/`delete`, mutable global/static access, calling
  an `extern "C"` function (an `extern "C"` declaration is never itself
  checked by any scpp compiler, see [§2.1](ch02-boundary-rules.md), so
  calling one always needs the same vouching `[[scpp::unsafe]]`
  provides for every other item on that list), calling a function
  that is itself declared `[[scpp::unsafe]]`, and calling through a
  function pointer whose type is itself `[[scpp::unsafe]]`-qualified
  (see [§5.16](ch05-static-checks.md#516-function-pointers)).
- **`span`'s bounds check ([§8](ch08-open-questions.md) Q1) and
  integer-overflow checking ([§5.8](ch05-static-checks.md)) are also
  relaxed inside an `[[scpp::unsafe]]` context**, for a different reason than the
  operations above: not because they're otherwise illegal (neither ever
  is -- both are ordinary, always-legal syntax), but because skipping a
  scpp-inserted *runtime* check carries none of the
  "corrupted-bookkeeping-leaks-into-surrounding-code" risk that keeps
  [§5.1-§5.4](ch05-static-checks.md) unconditional (see
  [§5.8](ch05-static-checks.md) for the full reasoning).

## 1.2 Marking an entire function: nesting vs. the function-level marker

There are two ways to give an entire function body broad, pervasive
access to [§5.5](ch05-static-checks.md)'s operations, and they answer two
different questions.

**Nesting a block** answers "does this function's *own implementation*
need broad access": wrap the *entire* function body in a single
`[[scpp::unsafe]] { }` block, nested one level inside it. This changes
nothing about who may *call* the function -- ordinary, unmarked code may
still call it freely, exactly like calling any other checked function:

```cpp
int legacy_style_function(int* p, size_t n) {
    [[scpp::unsafe]] {
        // the whole body lives here; §5.1-§5.4 still apply throughout
    }
}
```

**The function-level marker** answers a different question: "should
*calling* this function itself require the caller to be in an unsafe
context". Attach `[[scpp::unsafe]]` directly to the function's own
declaration (in the leading position, before its return type -- the same
position real C++ gives `[[noreturn]]`), and two things follow at once
([§1.3](#13-the-scppunsafe-attribute)): the function's entire body
becomes an unsafe context, exactly as the nested-block form above would
give it -- **and**, in addition, calling the function anywhere becomes
itself one of [§5.5](ch05-static-checks.md)'s gated operations, so an
ordinary, unmarked caller can no longer call it at all without wrapping
the call in `[[scpp::unsafe]] { }`:

```cpp
// Caller must ensure index is in bounds; nothing here can check that.
[[scpp::unsafe]] int get_unchecked(int* base, int index) {
    return base[index];   // no nested [[scpp::unsafe]] needed: the whole
                           // body is already an unsafe context, because
                           // of the attribute on the declaration above
}

int caller(int* arr) {
    [[scpp::unsafe]] {
        return get_unchecked(arr, 2);   // vouching for the call, not just
                                        // for a raw pointer dereference
    }
}
```

This exactly mirrors Rust's `unsafe fn`: an ordinary function that
happens to contain an internal `unsafe { }` block can still be called
from anywhere, because its author has already vouched for *that specific
internal operation* under *every* input the function accepts; `unsafe
fn` exists for the opposite case, where the function's soundness instead
depends on a precondition **the caller** must uphold (e.g. "`index` is
in bounds") that no amount of care inside the function's own body could
ever check on its own -- so Rust forces the obligation to propagate
outward, to every call site, instead of letting it stay hidden inside an
implementation detail. scpp's function-level `[[scpp::unsafe]]` is the
identical mechanism, spelled as an attribute rather than a keyword. Use
it exactly when a function's soundness is conditional on something only
the caller can guarantee; use a plain nested block instead when the
function's own implementation needs broad access but can still fully
vouch for itself under any input, so ordinary code should keep being
able to call it freely.

Neither form changes [§5.1-§5.4](ch05-static-checks.md)'s checking in any
way: ownership/move/alias/lifetime checking still applies to every
statement inside a function's body regardless of which form (if either)
made that body an unsafe context, exactly as
[§1.1](#11-what-scppunsafe-relaxes-and-what-stays-unconditional)
describes. In particular, scpp does **not** follow Rust's own
2024-edition tightening (`unsafe_op_in_unsafe_fn`), which additionally
requires an `unsafe fn`'s own body to re-wrap its unsafe operations in an
explicit inner `unsafe { }`: that Rust rule improves auditability inside
large functions but changes no soundness guarantee (an `unsafe fn` was
already fully gated at every call site regardless), so scpp does not
copy it -- a function-level `[[scpp::unsafe]]` gives its entire body an
unsafe context directly, with no re-wrapping required.

## 1.3 The `[[scpp::unsafe]]` Attribute

- **Grammar**: the attribute-token `unsafe` in the `scpp` attribute
  namespace, applied in either of two positions:
  - to an ordinary brace-delimited compound statement, via that
    statement's own attribute-specifier-seq: `[[scpp::unsafe]] { stmt;
    stmt; ... }`; or
  - to a function's own declaration or definition, via the leading
    attribute-specifier-seq before its return type: `[[scpp::unsafe]] int
    f(int* p) { ... }` (if a function is declared more than once, every
    declaration must repeat the attribute consistently -- see
    [§5.1](../../spec/en/01-unsafe.md#51-attributes-dclattrscppunsafe)
    (2) in the formal spec).

  This introduces **no new grammar whatsoever**: real C++ already gives
  every compound statement an optional leading attribute-specifier-seq
  (the same grammar slot `[[likely]] { ... }` uses), and already gives
  every function declaration/definition an optional leading
  attribute-specifier-seq (the same grammar slot `[[noreturn]]` uses), so
  `scpp::unsafe` is only a new attribute-token within existing
  productions, not a new keyword or a new grammar production -- unlike
  every other construct on this page's "no new keyword" claim, there's
  nothing here for a real C++ compiler to fail to parse, only an
  attribute it doesn't recognize (and silently ignores, or can be told to
  stop warning about via a flag like `-Wno-unknown-attributes`). v0.1
  only supports these two placements. Circle/Safe C++ also allows an
  `unsafe` prefix directly on a single condition expression, a match-arm
  body, or a constructor's mem-initializer; scpp defers all of those
  since it has no match-expressions or constructors yet -- they can be
  added as their own attribute placements later without changing
  anything specified here.
  - **The one placement that does *not* work**: an attribute-specifier-seq
    written immediately after a function's parameter list -- e.g. `int f(int*
    p) [[scpp::unsafe]] { ... }` -- attaches to the function's *type*
    ([dcl.fct]), not to the function itself and not to its body, and so
    has no effect at all; see
    [§1.2](#12-marking-an-entire-function-nesting-vs-the-function-level-marker)
    above for the two placements that do.
- **Scoping**: unlike Circle (where `unsafe { }` deliberately does *not*
  open a new lexical scope), scpp's `[[scpp::unsafe]] { }` is an
  **ordinary block**: it behaves exactly like a plain `{ }` compound
  statement -- locals declared inside go out of scope at the closing `}`,
  exactly as in ordinary C++. A `{ }` that silently scoped differently
  from every other `{ }` in the language would violate scpp's own "looks
  like C++, no surprises" rule. If a value computed inside
  `[[scpp::unsafe]] { }` needs to survive past it, declare it in the
  enclosing scope first and assign inside -- ordinary C++ already works
  this way. (The function-level marker introduces no new scope either --
  a function's body scope is unaffected either way.)
- **What gets unlocked**: exactly, and only, the operations listed in
  [§5.5](ch05-static-checks.md) become legal again inside an
  `[[scpp::unsafe]]` context: raw pointer dereference/arithmetic,
  `reinterpret_cast`/ incompatible C-style casts, untagged `union` member
  access, raw `new`/`delete`, mutable global/static access, calling an
  `extern "C"` function, calling a function itself declared
  `[[scpp::unsafe]]`, and calling through a function pointer whose type
  is itself `[[scpp::unsafe]]`-qualified (see
  [§5.16](ch05-static-checks.md#516-function-pointers)).
- **Also relaxed inside an `[[scpp::unsafe]]` context, for a different reason**:
  `span`'s bounds check ([§8](ch08-open-questions.md) Q1) and
  integer-overflow checking ([§5.8](ch05-static-checks.md)) are skipped
  too -- but unlike the operations above, neither is "illegal outside
  unsafe" (both are always legal syntax, everywhere). They're
  scpp-inserted *runtime* checks that happen to carry none of the
  "leakage into surrounding code" risk the next bullet explains, which is
  why they can join this list without contradicting it.
- **What stays exactly as strict**: everything in
  [§5.1-§5.4](ch05-static-checks.md) -- ownership/move state,
  alias-XOR-mutability, lifetime/dangling checks, and zero-init -- keeps
  running **unconditionally** through an `[[scpp::unsafe]]` context's
  statements, whichever of the two forms above established it.
  `[[scpp::unsafe]]` is a narrow, operation-level escape
  hatch, **not** a "stop checking this region" switch, and must not be
  equivalent to skipping ownership/borrow/lifetime checking for the
  block's statements -- only to relaxing the specific checks listed
  above. A `std::unique_ptr` can't be double-moved and alias-XOR-mutability
  can't be violated whether or not the code sits inside
  `[[scpp::unsafe]]`; otherwise, code right after the block (still
  under full checking) could observe corrupted state that was never
  actually proven sound. (Raw pointers themselves are still never
  move/borrow- tracked, inside or outside `[[scpp::unsafe]]` --
  that's unchanged; it's the same "raw pointers aren't a tracked type"
  rule that already applies everywhere.)
- **Nesting**: `[[scpp::unsafe]] { }` may nest inside another
  `[[scpp::unsafe]]` context (of either form) -- harmless, not an error.

---

[← Previous: Getting Started](ch00-design-philosophy.md) · [Table of Contents](README.md) · [Next: Basic Building Blocks →](ch02-boundary-rules.md)
