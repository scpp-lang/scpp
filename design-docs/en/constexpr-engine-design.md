# SCPP constexpr / consteval engine design

Status: research + first-review draft before implementation

## 0. Scope and headline

This document proposes how scpp should add a **general-purpose compile-time evaluation engine** for real C++-style `constexpr` and `consteval`, with the smallest architecture that can still solve the first urgent library blocker:

- a real typed `std::format_string<Args...>` / `std::print` / `std::println` surface, and
- compile-time rejection of format/argument mismatches the same way C++20/C++23 `<format>` does.[^format-string][^cpp-consteval]

The core recommendation is:

1. keep scpp's **real-C++ spelling rule**: reuse `constexpr`, `consteval`, `if consteval`, and `std::is_constant_evaluated()` semantics rather than inventing new syntax;[^spec-front-matter][^old-book-consteval]
2. implement compile-time evaluation as a **frontend interpreter**, not by lowering to LLVM IR or executing host machine code;[^clang-users][^gcc-constexpr][^rust-dev-guide]
3. integrate that interpreter into **generic-call monomorphization + semantic checking**, not into codegen;
4. explicitly ship a **documented v1 subset** that is already enough for format-string validation, constant-expression initializers, recursive helper functions, and straightforward compile-time parsers.

In short: **AST-resident compile-time interpreter, C++-compatible surface semantics, scpp-honest v1 subset, and a concrete path to `std::format_string<Args...>`.**

## 1. Current scpp starting point

### 1.1 Existing frontend pipeline

Today the compiler pipeline is still very direct:

1. parse to the AST,
2. monomorphize generic calls,
3. run move/dataflow checking,
4. lower to LLVM IR.[^driver-pipeline]

That is the right place to add compile-time evaluation: **after parsing, before codegen, while full type information is available**.

### 1.2 Existing generic machinery is close, but too narrow

The generic-call path already handles:

- ordinary type-parameter deduction from direct parameter positions like `T x`, and
- one special variadic base-class-deduction pattern for tuple-like recursive inheritance.[^movecheck-deduction][^movecheck-base-deduction][^old-book-generic-types]

But the current implementation is still fundamentally **single-pass and left-to-right**. It only binds a template pack when the pack appears in:

- a direct trailing function parameter pack, or
- the existing tuple-style base-class-deduction pattern.[^movecheck-deduction]

That fails for the motivating `<format>` shape:

```cpp
template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args);
```

What must happen is:

1. bind `Args...` from the **later** `args...` call arguments,
2. substitute that pack into the **earlier** parameter type `format_string<Args...>`,
3. then validate the first call argument against the now-concrete type.

The current monomorphizer cannot do step 2 because it tries to decide each parameter immediately as it walks past it.[^movecheck-deduction]

### 1.3 Existing AST and parser have no constexpr/consteval model yet

The lexer currently has no `KwConstexpr` / `KwConsteval` tokens, the AST `Function` node has no evaluation-mode flag, and statement parsing has no `if consteval` support.[^lexer-keywords][^ast-function]

The old book also explicitly documents the earlier design choice that scpp had `consteval` but intentionally omitted `constexpr` functions to avoid context-dependent behavior.[^old-book-consteval]

That historical rationale matters, but the same chapter also leaves room to revisit the choice if a real dual-use compile-time/runtime need appears. The `std::format_string<Args...>` case is exactly that need.[^old-book-consteval]

### 1.4 Existing constant-evaluation support is intentionally tiny and local

The compiler already evaluates one very small compile-time sub-language: non-type template arguments for variadic generic types (`int` literals, parameter names, and `+`). The code itself says this is **not a general consteval mechanism**.[^movecheck-non-type]

That existing helper is still valuable: it shows the codebase is already comfortable with *small, purpose-scoped interpreter logic in the frontend*. The new engine should generalize this, not replace it with an LLVM-side solution.

### 1.5 Existing module artifacts need expansion

Current `.scppm` writing keeps full source bodies only for exported generic definitions, via the optional `SGEN` block, and current `.scppm` reading explicitly ignores that block entirely.[^driver-sgen][^driver-scppm-read]

That is insufficient for imported `constexpr` / `consteval` code, and it is also not the right long-term architecture for modules. If user code imports `std::format_string`, the importer must be able to evaluate the exported constructor body and its private helper functions during semantic checking. The settled direction of this design is therefore: compile-time-relevant definitions should travel through `.scppm` as **structured serialized AST**, not as source snapshots to be reparsed later.

## 2. Research findings from mature systems

## 2.1 Real C++ surface semantics to copy

The C++ model scpp should follow is straightforward at the surface level:

- `consteval` declares an **immediate function**: every potentially-evaluated call must produce a constant expression, otherwise the program is ill-formed.[^cpp-consteval]
- `constexpr` declares a function that **may** be evaluated at compile time in constant-expression contexts, but may also run normally at runtime.[^cpp-constexpr]
- `if consteval` branches on whether evaluation is happening in a manifestly constant-evaluated context; the true branch is an immediate-function context.[^cpp-if]
- `std::is_constant_evaluated()` exposes the same distinction to ordinary code.[^cpp-is-constant]
- C++ constant expressions reject many classes of UB during compile-time evaluation: calling a non-`constexpr` function, overflowing a checked arithmetic operation, division by zero, out-of-bounds reads, exceeding implementation-defined evaluation limits, and so on.[^cpp-constant-expression]

For scpp, the most important behavioral split is:

- **`consteval` failure is always a compile error at the call site.**
- **`constexpr` failure is only a compile error when the surrounding context requires a constant expression.** Otherwise the call remains a runtime call.

That split is exactly what makes `std::format_string<Args...>` possible without forcing every formatting helper to be compile-time-only.

## 2.2 How real compilers implement constexpr

The implementation pattern in mainstream compilers is also clear:

- Clang's long-standing frontend implementation is the AST interpreter in `ExprConstant.cpp`, with an evaluation context, virtual call stack, and configurable depth/step limits.[^clang-users][^clang-exprconstant]
- GCC's implementation in `cp/constexpr.cc` likewise uses frontend evaluation with explicit recursion, loop, and operation limits (`-fconstexpr-depth`, `-fconstexpr-loop-limit`, `-fconstexpr-ops-limit`).[^gcc-constexpr]

Clang has also grown a newer bytecode-oriented constant interpreter for some paths, but that is an optimization/maintenance evolution inside Clang, not the minimum pattern scpp should start with.[^clang-constant-interpreter]

The architectural lesson for scpp is:

- **do not** compile to native code and execute it;
- **do not** wait until LLVM IR exists;
- **do** interpret frontend structures with compiler-owned memory, compiler-owned budgets, and compiler-owned diagnostics.

That matches scpp's existing architecture far better than an IR or JIT solution.

## 2.3 Adjacent language designs worth learning from

### Rust

Rust's const evaluation is deliberately stricter than normal execution, and the compiler runs it using a **virtual machine over MIR**, not host execution.[^rust-reference][^rust-dev-guide][^rust-interpreter]

Two Rust lessons are especially relevant:

1. **target-model evaluation matters** — Rust documents that compile-time interpretation happens in the environment of the compilation target, not the host.[^rust-reference]
2. **an interpreter is still the right abstraction even when it runs over a lowered IR instead of the AST.**

scpp should copy lesson (1) immediately, but for v1 keep the simpler AST-level implementation instead of introducing a MIR solely for constexpr.

### Zig

Zig's `comptime` is much more pervasive: arbitrary code can be forced to run at compile time, compile-time-known values are deeply integrated into the type system, and the same language surface is used for metaprogramming.[^zig-comptime]

The good lesson is ambition: Zig shows that compile-time execution is most useful when it can run ordinary control flow and helper functions.

The bad fit for scpp is syntax and philosophy:

- Zig's model is explicitly `comptime`-centric,
- scpp's design rule is **no new syntax unless the C++ surface truly cannot express it**.[^spec-front-matter]

So scpp should borrow **the “ordinary code, ordinary control flow” instinct**, but not the Zig surface model.

### D

D's CTFE is closer in spirit to what scpp needs: ordinary functions may be executed at compile time when the context requires it, subject to an implementation-defined supported subset.[^d-ctfe]

The relevant lesson is not D's exact semantics. It is the product strategy: **shipping a useful, documented subset first is acceptable** as long as the unsupported parts fail clearly.

## 3. Design principles for scpp

From the research and current codebase, the constexpr design should follow these rules.

1. **Reuse standard C++ spellings.**
   scpp should accept `constexpr`, `consteval`, `if consteval`, and `std::is_constant_evaluated()` with semantics intentionally modeled on real C++.[^cpp-consteval][^cpp-if][^cpp-is-constant]

2. **Keep evaluation in the frontend.**
   The interpreter operates over scpp AST + resolved type information and finishes before LLVM codegen.

3. **Be explicit about the v1 subset.**
   Like other scpp features, constexpr support should be honest about what is implemented now and what is deferred.

4. **Use target semantics, not host semantics.**
   Integer width, pointer size, null representation assumptions, and overflow behavior must match the selected target triple, following Rust's const-eval rule.[^rust-reference]

5. **Treat compile-time UB as a semantic error whenever the engine can see it.**
   If evaluation is required and the interpreter detects overflow, division by zero, out-of-bounds access, use-after-lifetime, or forbidden unsafe behavior, compilation fails.

6. **Ban `[[scpp::unsafe]]` from v1 constant evaluation.**
   The first implementation should not attempt “unchecked compile-time execution”. Encountering an unsafe function or unsafe block during required constant evaluation is a compile error.

7. **Preserve scpp's artifact split.**
   `.scppm` remains the compile-time artifact boundary; `.scppa` remains the native-code artifact boundary. But `.scppm` must now carry compile-time bodies for exported constexpr-relevant definitions.

## 4. Recommended top-level design

## 4.1 Surface syntax and AST additions

Add the following frontend representation changes.

### Lexer / parser

Add tokens for:

- `constexpr`
- `consteval`

and extend `if` parsing to recognize:

- `if consteval { ... } else { ... }`
- `if !consteval { ... } else { ... }`

### AST

Add a function evaluation-mode enum:

```cpp
enum class FunctionEvalMode {
    RuntimeOnly,
    Constexpr,
    Consteval,
};
```

and store it on `Function`.

Also add:

- `bool is_constexpr` on variable declarations that spell `constexpr`, and
- an `IfMode` on `Stmt` (`RuntimeIf`, `ConstexprIf`, `ConstevalIf`, `NotConstevalIf`) rather than inventing a separate statement kind.

That keeps the AST evolution small and preserves existing `StmtKind::If` handling structure.[^ast-function][^ast-stmt]

## 4.2 v1 supported subset

### In scope for v1

The first engine should support compile-time evaluation of:

- scalar literals and scalar locals:
  - `bool`, `char`, `int`, `long`, unsigned integer variants already modeled by the frontend;
  - `float`, `double`, and the project's existing floating-point scalar family where already modeled by the frontend/codegen;
- string literals, represented as immutable static byte arrays but still surface-typed as `const char*` for compatibility with current scpp typing/codegen behavior;[^movecheck-string][^codegen-string]
- null pointers and pointers into immutable static storage created by string literals or constexpr globals;
- fixed-size arrays of constexpr-compatible element types;
- read-only `std::span<const T>` values derived from arrays or string literals;
- trivial `struct` values whose fields are constexpr-compatible;
- selected `class` values if all of the following hold:
  - every field is constexpr-compatible,
  - construction happens through a `constexpr` or `consteval` constructor,
  - no destructor execution is required in v1,
  - no field is `mutable`,
  - no unsafe operation is used;
- control flow:
  - block
  - local declaration
  - assignment
  - `if`
  - `while`
  - `return`
  - recursion
- ordinary function calls to `constexpr` and `consteval` functions;
- floating-point arithmetic and comparison in checked constant-expression contexts, with compile-time rejection for engine-visible invalid operations such as division by zero in the same spirit as ordinary constant-expression failure;[^cpp-constant-expression]
- compile-time evaluation of expanded folds / pack-expanded helper code **after monomorphization**.

That subset is enough for:

- format-string scanning and placeholder parsing,
- bounds-checked character walking over string literals,
- recursion-based helper libraries,
- immediate constructors such as `format_string<Args...>("{}")`.

### Explicitly out of scope for v1

v1 should reject required constant evaluation of:

- `[[scpp::unsafe]]` blocks or unsafe functions;
- `extern "C"` calls, module-extern calls whose body is unavailable, and all FFI;
- `new`, `delete`, heap allocation, smart-pointer allocation helpers, and any dynamic lifetime management;
- union reads/writes;
- mutation through raw pointers;
- virtual dispatch or any runtime-only dynamic-type mechanism;
- lambdas (can come later once closure objects and capture lifetimes are modeled in the interpreter);
- exceptions / throwing behavior;
- I/O, threading, synchronization, environment access, filesystem access;
- compile-time execution that needs a destructor to run user code;
- loops or recursion that exceed the engine budget.

### Budgets

Adopt explicit hard limits modeled on Clang/GCC practice:[^clang-users][^gcc-constexpr]

- max call depth: **512**
- max total evaluation steps: **1,000,000**
- max iterations of one loop: **262,144**

These should be constants in v1, not user-facing CLI flags yet.

## 4.3 Pack-deduction fix for `format_string<Args...>`

### Existing failure mode

Today `monomorphize_generic_function_call` walks the parameters left-to-right and tries to decide each one immediately.[^movecheck-deduction]

That works for:

- `template<typename T> void f(T x)`
- `template<typename... Args> void f(Args&&... args)`
- tuple-like base-class deduction

but fails for:

```cpp
template<typename... Args>
void print(format_string<Args...> fmt, Args&&... args);
```

because parameter 0 depends on a pack only bound by parameter 1.

### Concrete replacement algorithm

Refactor full-header-form generic-call deduction into a three-stage solver.

#### Stage A: seed explicit arguments

Exactly as today:

- bind explicit non-pack type arguments,
- bind explicit non-type arguments,
- collect explicitly supplied pack elements if present.

#### Stage B: scan parameters and emit constraints

Replace the current “bind immediately or give up” logic with a constraint collector.

For each parameter/argument pair, emit one of:

1. **Direct type binding**
   - pattern `T`
   - bind `T := arg_type`

2. **Direct non-type binding**
   - existing integer-only non-type logic

3. **Direct function-parameter-pack binding**
   - pattern `Args&&... args`
   - bind `Args... := [arg_type_0, arg_type_1, ...]`

4. **Base-class deduction binding**
   - keep the current tuple-like logic as one specialized constraint producer

5. **Deferred compatibility obligation**
   - parameter type depends on template parameters or template packs, but this parameter is **not itself** a deduction source
   - record `(param_index, parameter_type_pattern, argument_expr)` to be checked later

`format_string<Args...>` is exactly case (5).

#### Stage C: finalize bindings, then re-check deferred parameters

After the whole call has been scanned:

1. ensure every non-pack template parameter is bound;
2. finalize pack bindings;
3. substitute the final bindings into each deferred parameter type;
4. run ordinary parameter compatibility checking on the deferred pair.

For `format_string<Args...>`, step (3) produces a concrete type such as:

```cpp
format_string<int, const char*>
```

and step (4) then checks whether the actual first argument can initialize that type — which is where the `consteval` constructor fires.

### Concrete integration with current code

This is not a greenfield rewrite. It should extend the exact code paths already in place.

1. Keep `parse_param_list`'s existing rule that a true function parameter pack must still be syntactically last.[^parser-pack]
2. Keep `deduce_via_base_class_chain` unchanged as one constraint producer.[^movecheck-base-deduction]
3. Replace the current body of `monomorphize_generic_function_call` with:
   - `collect_template_constraints(...)`
   - `solve_template_constraints(...)`
   - `check_deferred_template_obligations(...)`
4. Add a new helper:

```cpp
Type substitute_type_pack(const Type& pattern,
                          std::string_view pack_name,
                          const std::vector<Type>& pack_elems);
```

This is the missing piece for earlier dependent parameter types. The AST already has `Type::template_args` plus the `is_pack_expansion` sentinel for exactly this style of symbolic pack reference; the current code just does not substitute it in arbitrary parameter types yet.[^ast-pack-expansion]

### Why this is the right scope

This fix solves the motivating `format_string<Args...>` case **without** opening the door to arbitrary C++ template deduction complexity.

It still intentionally does **not** attempt:

- partial ordering between unrelated template overloads,
- recursive “peel one argument and recurse” deduction inside function bodies,
- deduction from arbitrary user-defined conversions.

It only adds the missing ability to **bind first, substitute later**.

## 4.4 Evaluator architecture

## 4.4.1 Core choice

**Decision: implement v1 as a tree-walking AST interpreter in the frontend.**

Rejected alternatives:

- **LLVM IR interpretation / JIT** — too late in the pipeline, host-environment leakage risk, much worse diagnostics, and poor fit for semantic-phase failures.
- **New MIR/bytecode just for constexpr v1** — plausible later, but too much infrastructure for the first cut.
- **Ad hoc per-feature evaluators** — would recreate the current non-type-argument one-off problem at a larger scale.

## 4.4.2 Module placement

Add a new frontend module, e.g. `src/constexpr.cppm`, exporting:

- `ConstValue`
- `ConstexprError`
- `ConstexprEngine`
- small helper APIs such as `evaluate_required_constant_expression(...)` and `evaluate_immediate_call(...)`

The engine is used by:

- generic-call monomorphization,
- movecheck/semantic checking,
- module interface loading (to know which bodies must stay available),
- later constant-expression sites such as `constexpr` variables and array bounds.

This is a **new subsystem used during semantic checking**, not a new backend pass.

## 4.4.3 Runtime model inside the interpreter

The interpreter needs four things.

### 1. Compile-time values

Use a tagged union:

```cpp
enum class ConstValueKind {
    Void,
    Bool,
    Int,
    Char,
    Pointer,
    Array,
    Struct,
    Class,
    Span,
};
```

with target-aware payloads.

### 2. Virtual storage

Represent memory using compiler-owned objects:

- globals / string literals: immutable static storage
- one frame per function call
- one slot per local / parameter
- subobject access for fields and array elements

Pointers are not raw host addresses. They are `(storage_id, offset, pointee_type)` references into this virtual storage.

### 3. Evaluation context

Track:

- current mode (`RuntimeCheckOnly`, `RequiredConstexpr`, `ImmediateConsteval`)
- call stack frames
- step counter
- loop-iteration counter
- recursion depth
- current source location for diagnostics

### 4. Result cache

Cache successful evaluation of:

- constexpr variable initializers,
- immediate constructor calls on string literals,
- monomorphized helper functions called repeatedly by the format validator.

This cache should be keyed by `(function name, concrete argument values/types, target triple)`.

## 4.4.4 Module-interface impact

Current `.scppm` support is generic-only and currently unread on import.[^driver-sgen][^driver-scppm-read]

The settled design is to replace that source-snapshot direction with **structured AST serialization** from the outset.

### Structured AST payload

`.scppm` should gain a structured “compile-time AST” section containing serialized AST nodes, not source text. That section should store:

1. exported `constexpr` and `consteval` definitions;
2. exported generic definitions;
3. any private helper definitions, type definitions, and constant initializers reachable from those exported compile-time-relevant definitions;
4. enough symbol/type metadata to reconnect the deserialized nodes to the importing module's semantic environment without reparsing source text.

### Why structured AST now

This is the more ambitious v1 path, but it is the right one.

- It matches the architecture mature module implementations converge on: imported templates and constexpr-evaluable entities are carried as compiler-readable structured frontend state, not as raw source text snapshots to be reparsed by every importer.
- It avoids baking a header-era “reparse source in every consumer” model into scpp's module system just as the language is adding its first real cross-module compile-time execution story.
- It gives generics and constexpr bodies the same long-term representation boundary.

### Recommendation on coexistence vs unification

This document recommends **unification, not coexistence**:

- the new structured serialization mechanism should become the cross-module representation for both constexpr-evaluable bodies **and** scpp's existing exported generic bodies;
- the current `SGEN` source snapshot path should be treated as a transitional implementation detail of the reference compiler, not as a second permanent module-interface mechanism.

The reason is architectural, not cosmetic. Imported generic monomorphization and imported constexpr evaluation are both consuming frontend body graphs across a module boundary. Maintaining one source-snapshot path for generics and one structured-AST path for constexpr would duplicate reachability rules, versioning, deserialization, and semantic reattachment logic for two pieces of functionality that are fundamentally the same kind of payload.

## 4.5 Diagnostics and error taxonomy

### Recommendation: add `ConstexprError`

`ParseError`, `DataflowError`, `CodegenError`, and `DriverError` map cleanly to the current pipeline.[^parser-error][^movecheck-error][^codegen-error][^driver-error]

Compile-time evaluation failures are different enough to deserve their own kind:

```cpp
struct ConstexprError : std::runtime_error {
    SourceLocation loc;
    std::vector<ConstexprFrame> stack;
};
```

Why a new error kind is better than reusing `DataflowError`:

- the failure may arise from perfectly type-correct code whose only issue is “not evaluable here”;
- users benefit from a constexpr stack trace (“while evaluating `parse_replacement_field` called from ...”);
- it cleanly separates semantic movement/borrow failures from compile-time execution failures.

The existing CLI/project diagnostic printer already knows how to print `SourceLocation`; it only needs one more catch block and an optional note-emission path.[^cli-diagnostic]

### Required diagnostics in v1

When constant evaluation is required, the engine should issue hard errors for engine-visible cases such as:

- call to non-`constexpr` function,
- call to `consteval` in a non-immediate context,
- arithmetic overflow in checked code,
- division/modulo by zero,
- out-of-bounds array/span/string access,
- reading an uninitialized virtual slot,
- forbidden unsafe operation,
- step/depth/loop-limit exhaustion,
- missing imported constexpr body.

Where C++ itself says “ill-formed, no diagnostic required”, scpp may still diagnose whenever its engine can see the problem. That is consistent with Clause 4's general “issue at least one diagnostic” direction and with scpp's broader safety-first design.[^spec-front-matter][^cpp-constexpr][^cpp-constant-expression]

## 4.6 `constexpr` vs `consteval` semantics in scpp

### `consteval`

- spelled exactly `consteval`
- every potentially-evaluated call is immediate
- selected overload must be evaluable at compile time
- failure is a compile error at the call site
- may call other `consteval` or `constexpr` functions during evaluation
- cannot appear on destructors, allocation functions, or deallocation functions, following C++.[^cpp-consteval]

### `constexpr`

- spelled exactly `constexpr`
- function is eligible for compile-time evaluation
- if the surrounding context requires a constant expression, evaluation must succeed
- otherwise the call remains an ordinary runtime call
- still participates in overload resolution as an ordinary function declaration

### `if consteval`

- parsed and represented explicitly in the AST
- during compile-time interpretation, the interpreter executes the appropriate branch
- the true branch creates an immediate-function context, matching C++23.[^cpp-if]

### `std::is_constant_evaluated()`

Treat this as a compiler-known library intrinsic in v1:

- during compile-time interpretation: returns `true`
- in ordinary runtime codegen: returns `false`

That is enough for the standard-library use cases and mirrors common C++ implementations, which often define it in terms of `if consteval` or a compiler builtin.[^cpp-is-constant]

## 5. Worked example: `std::format_string<Args...>`

A scpp-friendly v1 library sketch can look like this:

```cpp
export module std;

export namespace std {

template<typename... Args>
class format_string {
    const char* text_;

public:
    consteval format_string(const char* s)
        : text_(s) {
        detail::validate_format<Args...>(s);
    }

    constexpr const char* get() const {
        return this->text_;
    }
};

template<typename... Args>
void print(format_string<Args...> fmt, Args&&... args);

template<typename... Args>
void println(format_string<Args...> fmt, Args&&... args);

} // namespace std
```

Now consider:

```cpp
int x = 1;
const char* name = "scpp";
std::print("{} {}", x, name);
```

Under the proposed design:

1. parser builds the full-header generic `print` template normally;
2. generic-call monomorphization scans the call;
3. parameter 1 (`Args&&... args`) binds `Args... := [int, const char*]`;
4. parameter 0 is recorded as a deferred compatibility obligation with pattern `format_string<Args...>`;
5. after binding finalization, the earlier parameter type becomes `format_string<int, const char*>`;
6. the first argument is a string literal (`const char*` in today's scpp type model);[^movecheck-string][^codegen-string]
7. ordinary parameter checking sees that this can initialize `format_string<int, const char*>` only by calling its constructor;
8. because that constructor is `consteval`, the compiler invokes `ConstexprEngine::evaluate_immediate_call`;
9. inside that immediate call, `detail::validate_format<int, const char*>(s)` runs at compile time;
10. if the parser finds two automatic replacement fields and the argument pack length is two, evaluation succeeds and the call remains well-formed.

If instead the program writes:

```cpp
std::print("{} {} {}", x, name);
```

then step 9 throws `ConstexprError`, with the primary diagnostic at the call site and notes from the virtual constexpr stack, e.g.:

```text
main.scpp:12:11: error: consteval construction of 'std::format_string<int, const char*>' failed
note: while evaluating 'std::detail::validate_format<int, const char*>'
note: format string expects 3 arguments, but call supplies 2
```

That is the exact motivating gap solved end-to-end.

## 6. Phased implementation plan

## Phase A — syntax, AST, and serialization schema

1. add lexer tokens for `constexpr` / `consteval`
2. add `FunctionEvalMode`, variable `is_constexpr`, and `if consteval` AST flags
3. parse function/constructor declarations carrying those specifiers
4. define a versioned structured `.scppm` AST payload format for compile-time-relevant definitions
5. mark exported generic / `constexpr` / `consteval` roots and compute the reachable helper/type graph that must be serialized with them

Merge/testability:

- parser tests
- AST serialization-schema tests
- reachability tests over exported compile-time definitions
- no evaluator yet

## Phase B — structured `.scppm` serialization / deserialization

1. serialize structured AST nodes for exported generic and constexpr-relevant definitions instead of source snapshots
2. deserialize those AST nodes on import and reattach symbol/type identity in the importing compiler session
3. replace the current generics-only source snapshot path with the structured representation
4. preserve backward rejection with a clear diagnostic for older `.scppm` files that do not carry the required structured payload
5. add focused cross-module tests for imported generic bodies and imported `consteval` helpers

Merge/testability:

- module-interface round-trip tests
- imported generic body tests
- imported constexpr body availability tests

## Phase C — generic pack-deduction refactor

1. refactor full-header-form generic-call monomorphization into constraint collection + solving
2. add deferred compatibility obligations
3. add `substitute_type_pack(...)` for earlier dependent parameter types
4. preserve existing tuple/base-class deduction as one constraint producer
5. add focused tests for:
   - `template<typename... Args> void f(F<Args...>, Args&&...);`
   - explicit + deduced mixed arguments
   - failure diagnostics when the earlier parameter becomes incompatible after substitution

Merge/testability:

- no constexpr engine required yet
- this phase alone should make the `format_string<Args...>` parameter shape resolvable

## Phase D — minimal constexpr / consteval engine

1. add `ConstexprError`, `ConstValue`, `ConstexprEngine`
2. support scalar locals including floating-point values, string literals, arrays, read-only spans, `if`, `while`, `return`, recursion
3. support checked floating-point arithmetic/comparison alongside integer arithmetic in constant-expression contexts
4. reject unsafe/FFI/dynamic-allocation paths explicitly
5. wire `consteval` calls into semantic checking
6. add budget exhaustion diagnostics

Merge/testability:

- immediate functions over ints/chars/floats/strings
- compile-time string parser micro-tests
- imported-module consteval helper tests

## Phase E — `constexpr` proper and required constant-expression contexts

1. allow `constexpr` functions and constructors
2. add required constant-expression checking for:
   - `constexpr` variable initializers
   - array bounds once scpp adopts constexpr-sized arrays uniformly
   - non-type template arguments (replacing the current tiny one-off evaluator)
3. implement `if consteval`
4. implement `std::is_constant_evaluated()` intrinsic semantics

Merge/testability:

- same function callable both at compile time and runtime
- branch-selection tests for `if consteval`
- floating-point constexpr variable / helper tests

## Phase F — stdlib integration for `<format>` / `<print>`

1. add `std::format_string<Args...>`
2. port current runtime validator into constexpr-friendly helpers
3. route `std::print` / `std::println` through typed format-string parameters
4. preserve today's supported formatting subset first, but move the validation from runtime to compile time
5. add user-facing docs/book updates

Merge/testability:

- positive/negative blackbox tests for `std::print` / `std::println`
- imported `std` module tests

## Phase G — widening the constexpr subset

Later, if needed:

- constexpr lambdas
- more class construction/destruction
- more of the standard library
- richer pointer semantics
- optional bytecode/MIR layer if profiling shows AST-walking is a bottleneck

## 7. Final settled design decisions

User review has now resolved the remaining forks. The final design decisions are:

1. **Keep v1 class support at the originally recommended boundary:** constexpr-compatible fields plus `constexpr`/`consteval` constructors, but no user-defined destructor execution in v1.
2. **Adopt structured AST serialization immediately for `.scppm` compile-time payloads, rather than reusing source snapshots.**
3. **Unify generic-body and constexpr-body cross-module transport on that one structured serialization mechanism, instead of maintaining two permanent mechanisms.**
4. **Include floating-point constexpr arithmetic in v1, not in a later widening phase.**

## 8. Final recommendation

The practical recommendation is:

1. **land structured AST serialization/deserialization for compile-time-relevant `.scppm` payloads first**;
2. **land the pack-deduction refactor second**;
3. **land a frontend AST interpreter for `constexpr` / `consteval` third, with floating-point support already in v1**;
4. **use `std::format_string<Args...>` as the first proving example**;
5. **ship a documented subset instead of waiting for full C++ constexpr parity**.

That gives scpp the smallest architecture that is still genuinely general-purpose, matches real C++ semantics closely enough to be unsurprising, and directly unlocks the typed `std::print` / `std::println` story that motivated this design.

## Sources

[^spec-front-matter]: `scpp-reference/docs/spec/en/00-front-matter.md`, especially Clause 1 and Clause 4.
[^old-book-consteval]: `scpp-reference/docs/old-book/en/ch06-safe-subset.md:142-160`.
[^old-book-generic-types]: `scpp-reference/docs/old-book/en/ch05-static-checks.md:368-379, 473-516`.
[^driver-pipeline]: `scpp-reference/src/driver.cppm:660-701`.
[^driver-sgen]: `scpp-reference/src/driver.cppm:86-121`.
[^driver-scppm-read]: `scpp-reference/src/driver.cppm:211-243`.
[^lexer-keywords]: `scpp-reference/src/lexer.cppm:40-126,240-276`.
[^ast-function]: `scpp-reference/src/ast.cppm:553-724`.
[^ast-stmt]: `scpp-reference/src/ast.cppm:468-525`.
[^ast-pack-expansion]: `scpp-reference/src/ast.cppm:159-173`.
[^parser-error]: `scpp-reference/src/parser.cppm:17-28`.
[^movecheck-error]: `scpp-reference/src/movecheck.cppm:19-25`.
[^codegen-error]: `scpp-reference/src/codegen.cppm:35-41`.
[^driver-error]: `scpp-reference/src/driver.cppm:38-42`.
[^cli-diagnostic]: `scpp-reference/src/cli/cli.cppm:190-224`.
[^parser-pack]: `scpp-reference/src/parser.cppm:2253-2273`.
[^movecheck-deduction]: `scpp-reference/src/movecheck.cppm:6828-6935`.
[^movecheck-base-deduction]: `scpp-reference/src/movecheck.cppm:6202-6235`.
[^movecheck-non-type]: `scpp-reference/src/movecheck.cppm:6166-6200`.
[^movecheck-string]: `scpp-reference/src/movecheck.cppm:1368-1380`.
[^codegen-string]: `scpp-reference/src/codegen.cppm:2905-2915`.
[^format-string]: cppreference, `std::basic_format_string` — https://en.cppreference.com/cpp/utility/format/basic_format_string
[^cpp-consteval]: cppreference, `consteval` — https://en.cppreference.com/cpp/language/consteval
[^cpp-constexpr]: cppreference, `constexpr` — https://en.cppreference.com/cpp/language/constexpr
[^cpp-if]: cppreference, `if` / consteval if — https://en.cppreference.com/cpp/language/if
[^cpp-is-constant]: cppreference, `std::is_constant_evaluated` — https://en.cppreference.com/cpp/types/is_constant_evaluated
[^cpp-constant-expression]: cppreference, `constant expression` — https://en.cppreference.com/cpp/language/constant_expression
[^clang-users]: Clang Users Manual, constexpr limits — https://clang.llvm.org/docs/UsersManual.html#controlling-implementation-limits
[^clang-exprconstant]: LLVM/Clang source, `clang/lib/AST/ExprConstant.cpp` — https://github.com/llvm/llvm-project/blob/main/clang/lib/AST/ExprConstant.cpp
[^clang-constant-interpreter]: Clang Constant Interpreter docs — https://clang.llvm.org/docs/ConstantInterpreter.html
[^gcc-constexpr]: GCC C++ dialect options, `-fconstexpr-*` limits — https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Dialect-Options.html
[^rust-reference]: Rust Reference, constant evaluation — https://doc.rust-lang.org/reference/const_eval.html
[^rust-dev-guide]: Rust compiler dev guide, const eval overview — https://rustc-dev-guide.rust-lang.org/const_eval.html
[^rust-interpreter]: Rust compiler dev guide, interpreter over MIR — https://rustc-dev-guide.rust-lang.org/const_eval/interpret.html
[^zig-comptime]: Zig language reference, comptime — https://ziglang.org/documentation/master/#Comptime
[^d-ctfe]: D language spec, CTFE — https://dlang.org/spec/function.html#ctfe
