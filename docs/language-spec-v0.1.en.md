# Language Specification Draft v0.1 (codename TBD)

> A language that "looks exactly like idiomatic modern C++", adding only a very
> small set of extensions (the core being the `safe` keyword). Regions annotated
> with `safe` enable Rust-style sound compile-time safety checks; all other code
> follows ordinary C++ semantics. The backend generates native binaries via LLVM.

---

## 0. Design Philosophy (the immovable North Star)

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

## 1. Safety Context

The compiler maintains a **safety context** for every function body, lambda,
block, and type:

- **`unsafe` (default)**: ordinary C++ semantics. No ownership / borrow / alias
  checking.
- **`safe`**: all static safety checks enabled (see §4).

### 1.1 How the context is determined

- An unannotated function/block -> `unsafe` (default).
- A `safe`-annotated function/block -> `safe`.
- A nested block inherits its parent's context unless explicitly overridden.
- An `unsafe { }` block opens an escape hatch inside a `safe` context, restoring
  `unsafe` semantics.
- (Not yet supported) A `safe { }` block enabling checks inside an `unsafe`
  context — deferred to a later version.

### 1.2 Annotation positions

```cpp
safe int f(...);                 // free function
struct S { safe void g(); };     // member function
safe struct Widget { ... };      // type-level: all members safe by default (v0.2)
auto lam = safe [](int x){...};  // lambda (v0.2)
```

v0.1 **only requires function-level `safe` and block-level `unsafe { }`**. All
other annotation positions go to the backlog.

---

## 2. Boundary Rules (Safe <-> Unsafe interaction)

This is critical for soundness and must be strict.

| Call direction | Rule |
|----------------|------|
| `unsafe` calls `safe` | **Freely allowed**. A safe function is safe for any caller. |
| `safe` calls `safe` | Freely allowed; participates in checking normally. |
| `safe` calls `unsafe` | **Must be wrapped in `unsafe { }`**, otherwise a compile error. The programmer vouches for it. |
| Raw pointer deref in `safe` | Must be inside `unsafe { }`. |

- Data contracts at the boundary: references/pointers a safe function exposes to
  the unsafe world carry lifetime obligations that are **not enforced** on the
  unsafe side (the unsafe side is on its own). Conversely, a reference passed
  from unsafe into safe is **assumed valid for the duration of the call**
  (caller's obligation).
- The compiler should be able to mark whether an `unsafe` function has been
  "manually reviewed as safe to call" — v0.1 does not formalize this and relies
  on `unsafe { }` vouching.

---

## 3. Syntactic Sugar / Re-semantification of Existing Syntax

Inside a **safe context**, the following C++ spellings acquire strong static
semantics. In an unsafe context they keep their ordinary C++ meaning.

| C++ spelling | Semantics in a safe context |
|--------------|------------------------------|
| `std::move(x)` | Compiler builtin **move hint**. Marks `x` as *moved-out*. Reading `x` afterwards is an error until it is reassigned. Not an ordinary function call. |
| `T&` | Mutable borrow `&mut T`: exclusive, participates in alias-XOR-mutability and lifetime checking. |
| `const T&` | Shared borrow `&T`: multiple may coexist, but mutually exclusive with any `&mut`. |
| `T&&` (parameter) | Passed by move (ownership transfer). |
| `std::unique_ptr<T>` | Unique ownership. Move semantics fit naturally. |
| `std::shared_ptr<T>` | Shared ownership (refcounted). Allowed in safe, but aliasing follows interior-mutability rules (refined in v0.2). |
| `std::span<T>` / `std::string_view` | Borrowed views with a lifetime; checked for dangling. |
| local variable `T x;` | Owns its value; dropped (destroyed) at end of scope. |
| `new` / `delete` / raw `T*` | **Forbidden by default** in safe regions; require `unsafe { }`. |

**Key principle**: these semantic shifts are "invisible" to the user — they
still write familiar C++, they just get extra compile-time errors in safe
regions that block bugs.

---

## 4. Static Checks in Safe Regions (the soundness core)

Within a safe context, the compiler guarantees (for the supported subset) the
following properties:

### 4.1 Ownership & Move
- Every value has a unique owner.
- After `std::move(x)`, `x` enters the *moved-out* state; reading a moved-out
  value -> error.
- Reassignment returns a variable to the *initialized* state.
- At end of scope, values still *initialized* are dropped; moved-out values are
  not dropped.

### 4.2 Borrow & Aliasing
- **Alias XOR mutability**: at any instant an object may have either any number
  of `const T&` (shared borrows), or exactly one `T&` (mutable borrow), never
  both.
- While an active borrow exists, the borrowed object may not be moved or
  destroyed.

### 4.3 Lifetime
- A borrow must not outlive the borrowed value (**no dangling references**).
- v0.1 performs **intraprocedural borrow checking only**, based on NLL-style
  dataflow analysis (liveness-driven region inference).
- **No lifetime syntax is exposed.** Cross-function signatures use the following
  **elision defaults** (a simplified version of Rust's lifetime elision):
  - If a function has exactly one reference input parameter, all reference
    outputs borrow from its lifetime.
  - If there is a `this` and the function returns a reference, the output
    borrows from `this`'s lifetime.
  - Any other case that cannot be inferred -> error, advising "this signature is
    not yet supported; refactor or return by value / smart pointer". (No
    explicit annotation syntax is introduced in v0.1.)

### 4.4 Initialization
- Reading an uninitialized variable is forbidden.
- All paths must initialize before use (dataflow analysis).

### 4.5 Prohibited in Safe Regions (unless in `unsafe { }`)
- Raw pointer dereference, pointer arithmetic.
- `reinterpret_cast`, C-style casts to incompatible types.
- (Untagged) `union`.
- Raw `new` / `delete`.
- Access to mutable global/static variables.
- Calling a function not annotated `safe`.

---

## 5. The Safe Subset Supported in v0.1

Inside safe regions, **only** the following syntax is supported; everything else
reports `E-UNSUPPORTED-IN-SAFE` (explicitly distinct from "unsafe", meaning
"sound checking not yet implemented"):

**Types**
- Scalar primitives: `bool`, integers, floats, `char`.
- `struct` (with fields of supported types only).
- `std::unique_ptr<T>`, `std::vector<T>`, `std::span<T>`, `std::string_view`,
  `std::string` (minimal subset).

**Expressions / Statements**
- Local variable declaration and initialization.
- `&` / `const &` borrows.
- `std::move`.
- Function calls (callee must be `safe`, otherwise `unsafe {}`).
- Arithmetic / logical / comparison operators.
- `if` / `while` / `for` (incl. range-for) / `return`.
- Member access, subscript (`vector`/`span`, with bounds semantics — runtime
  check policy per §7).

**Not yet supported (safe-region backlog)**
- Templates / generics, `concept`.
- Inheritance, virtual functions.
- Exceptions.
- Lifetime checking of lambdas capturing references.
- The full aliasing model for `shared_ptr`.
- Complex cross-function lifetimes (cases requiring explicit annotations).

---

## 6. Compilation Pipeline (architecture)

```
source
 └─► Lexer
     └─► recursive-descent Parser ──► unified AST (one AST for safe/unsafe, carries a safety tag)
         └─► name resolution + type checking ──► HIR (desugar: std::move -> move hint, etc.)
             ├─ [unsafe region] ─────────────────────────► lower directly
             └─ [safe region] ─► MIR (CFG + three-address)
                            └─► borrow check (init / move / alias / lifetime)
                                └─► lower after checks pass
                 └─► LLVM IR ──► LLVM opt ──► target binary
```

Key points:
- **Unified AST**: safe and unsafe code share one AST, with a safety-context bit
  on nodes.
- **Borrow checking runs only on the MIR of safe regions**; unsafe regions skip
  it and lower directly.
- The frontend need only be "good enough" for unsafe/ordinary C++ — full C++
  compatibility is not pursued.
- MIR makes things explicit: ownership transfers, borrow start/end, drop
  insertion points, CFG.

---

## 7. Open Questions (to be decided later)

1. **Out-of-bounds subscript**: in safe regions, does `vector[i]` / `span[i]`
   insert a runtime bounds check (like Rust), or require a checked API?
   Leaning: insert bounds checks by default in safe regions.
2. **Integer overflow**: does safe check signed overflow? Leaning: panic in
   debug, wrapping/UB in release? TBD.
3. **Panic model**: how do OOB / assertion failures terminate? `std::terminate`
   or a custom panic + stack unwinding? v0.1 uses `std::terminate` (abort).
4. **Interior mutability**: introduce a `Cell`/`RefCell` equivalent to carry
   legal mutable aliasing?
5. **`safe` vs `const`**: how does a `const` member function map to borrows in a
   safe region?
6. **ABI / interop with existing C++ libraries**: how to engineer safe code
   calling third-party headers (all unsafe) — treat them all as `unsafe`?
7. **Language/compiler name, file extension.**

---

## 8. MVP Milestones (implementation order, end-to-end first)

- **M0**: Freeze this spec + choose the implementation language and LLVM
  bindings. **(current stage)**
- **M1**: Minimal end-to-end. Subset: scalars + locals + `if`/`while` +
  functions -> AST -> LLVM IR -> executable with correct return value. **No safe
  checks yet**; get the front/back ends connected first.
- **M2**: Type system + `struct` + `unique_ptr` + move semantics (`std::move` as
  a hint); implement **move-out checking** (the simplest sound check).
- **M3**: Build MIR + initialization checking + drop insertion.
- **M4**: Borrow & alias-XOR-mutability checking (intraprocedural).
- **M5**: NLL-style lifetime inference + dangling-reference checking
  (intraprocedural) + elision rules.
- **M6**: `vector`/`span`/`string_view` support + bounds-check policy +
  diagnostic quality.
- **M7+**: Generics/templates, traits/concepts, cross-function lifetimes,
  standard-library expansion, incremental compilation.

---

## 9. Reference Implementations (required reading)

- **Circle** (Sean Baxter): a C++ superset with a borrow checker — almost the
  same path as this design.
- **Hylo / Val**: mutable value semantics, an alternative that sidesteps
  borrow-checking complexity.
- **cppfront / cpp2** (Herb Sutter): C++ syntax renewal + safe defaults.
- **Rust `rustc` borrowck + the Polonius papers / NLL RFC**: the mature
  implementation of borrow checking.
- **MLIR**: if the backend wants a high-level dialect to carry ownership/lifetime
  before gradually lowering.

---

*Status: draft v0.1 · pending review before entering M1.*
