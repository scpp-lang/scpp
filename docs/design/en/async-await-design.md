# SCPP async/await / coroutine design

Status: refreshed against current `main`; design draft for review before implementation

## 0. Scope and headline

This document proposes how scpp should add **C++20/23-style coroutines**: `co_await`, `co_yield`, `co_return`, the promise-type protocol, coroutine handles, and a minimal library layer (`task`, `generator`, executor helpers).

The design is grounded first in scpp's settled language principles and current normative text, especially:

- C++-style surface syntax and semantics by default, unless scpp has a major gap that genuinely needs a new rule
- explicit-over-implicit design, including scpp's general refusal to lean on ADL-shaped customization paths
- thread-safety as a first-class dual mechanism: structural derivation plus explicit contracts (`docs/spec/en/04-thread-safety-properties.md`, especially 8.5)
- the current inheritance/interface model, including mandatory virtual destructors for `class` and two-word interface representations (`docs/spec/en/11-inheritance-and-interfaces.md`)

Comparisons to C++, Rust, and other coroutine-capable languages are used here only as inputs and contrasts; they do **not** override scpp's own design goals, and current compiler implementation status is treated as a feasibility/input signal rather than the criterion for whether the language design is sound.

## 0.1 Relationship to today's docs/spec layout

- `docs/missings.md` still records that scpp has no async/await, futures, or streams story yet.
- `docs/open_issues.md` currently contains no async-specific blocker, though its note about thread-safety structural derivation ignoring inherited ordinary bases is relevant if coroutine wrapper types tried to use ordinary inheritance.
- `docs/spec/en/README.md` still has no coroutine clause today. When this design is eventually formalized, it should avoid hard-coding stale future clause numbers and instead be mapped onto the then-current spec layout.

## 1. Feasibility summary

### 1.1 Confirmed

1. **LLVM already has the right lowering substrate.**
   Local LLVM 22 exposes the standard switched-resume coroutine intrinsics in `/usr/lib/llvm-22/include/llvm/IR/Intrinsics.td`:
   `llvm.coro.id`, `alloc`, `size`, `begin`, `save`, `suspend`, `free`, `end`, `resume`, `destroy`, `promise`.

2. **`opt-22 -passes=coro-early,coro-split` really does split a pre-split coroutine into ramp/resume/destroy functions.**
   A local hand-written `minimal-coro.ll` using `llvm.coro.*` lowered into a frame type plus `f.resume`, `f.destroy`, and `f.cleanup` functions. So scpp does **not** need to hand-build the final state machine itself.

3. **Clang still uses those intrinsics internally.**
   With `clang++-22 -Xclang -disable-llvm-passes -S -emit-llvm`, a simple coroutine emitted `presplitcoroutine` IR containing `llvm.coro.id/alloc/begin/save/suspend/free/end/promise`. Without `-disable-llvm-passes`, clang emitted the already-split state machine, which is consistent with clang running coroutine lowering before final IR emission.

4. **Real C++ promise/awaiter semantics behave as expected.**
   Local experiments confirmed:
   - `co_return expr` requires `promise_type::return_value(expr-type)`
   - `co_return;` requires `promise_type::return_void()`
   - `co_yield expr` requires `promise_type::yield_value(expr-type)`
   - the promise hooks are called in the expected order: `get_return_object`, `initial_suspend`, body, `return_*`, `final_suspend`
   - `co_await` consults `promise.await_transform(...)` first when present
   - awaiter protocol is `await_ready`, `await_suspend`, `await_resume`
   - `await_suspend` may return `void`, `bool`, or `std::coroutine_handle<>`

5. **Rust confirms the core soundness hazard, but also clarifies that scpp's chosen ABI differs in an important way.**
   Local `rustc` experiments confirmed:
   - `Future::poll` requires `Pin<&mut Self>`
   - `async fn` futures are `!Unpin`
   - moving a manually-built self-referential value invalidates its internal self-pointer
   - `Pin::into_inner` is rejected for `PhantomPinned` values

### 1.2 Most important conclusion about frame stability

Rust is a useful contrast here, but not the baseline design target.

Rust needs `Pin` because the **future value itself is the state object**, so moving the future can move self-referential state.

For scpp's proposed **C++ coroutine model**, the source-level coroutine returns a **handle to a separately allocated coroutine frame** (the LLVM/clang switched-resume ABI). Moving the user-visible `task<T>` / `generator<T>` object moves only the handle, **not** the frame.

**Therefore: scpp probably does _not_ need a user-visible `Pin` equivalent for internal self-borrows inside a coroutine frame.**

That is the single biggest design simplification.

### 1.3 Main uncertainty

The remaining hard problem is **not frame self-reference**. It is:

> how to soundly handle **borrows of data outside the frame** that remain live across a suspension point.

This document recommends a conservative phase-1 answer:

- **allow internal borrows across suspension** when both the referent and the reference live in the coroutine frame
- **reject external borrows across suspension** for phase 1
- revisit borrow-carrying coroutine return objects later if needed

I think this is the right first implementation boundary for scpp's current movechecker.

## 2. Grounding in scpp's current implementation model (non-normative)

This section is about implementation fit and staging, not about whether the language design is semantically justified.

Today the relevant pipeline is:

```text
source
  -> lexer
  -> recursive-descent parser
  -> unified AST
  -> MIR builder (CFG + statements)
  -> move/borrow/lifetime checker over MIR
  -> direct LLVM IR codegen from AST
  -> object emission / linking
```

Grounded details from the real codebase:

- `src/lexer.cppm` has no coroutine tokens today.
- `src/ast.cppm` has no coroutine AST nodes today; statements stop at `VarDecl/Return/If/While/Break/Continue/ExprStmt/Block` and expressions stop at existing runtime/generic/lambda forms.
- `src/mir.cppm` builds CFG MIR for movecheck only; it currently has no suspend-point notion.
- `src/movecheck.cppm` is intraprocedural and NLL-style: it computes `live_after` sets for reference locals and releases borrows after last use.
- `src/codegen.cppm` still lowers ordinary functions directly from AST and uses lexical scope stacks + explicit destructor emission.
- `src/driver.cppm` runs `monomorphize_generics(program)`, then `check_moves(program)`, then codegen.

Implication:

- parser/AST/MIR/movecheck all need coroutine awareness
- codegen cannot stay entirely unchanged because coroutine lowering needs suspend/resume/destroy paths
- but LLVM can still do the final frame splitting/state-machine rewrite

## 3. Proposed surface language design

### 3.1 Syntax

Adopt the real C++ coroutine spellings unchanged:

- `co_await expr`
- `co_yield expr`
- `co_return expr;`
- `co_return;`

This is consistent with the project's stated design philosophy: these are **existing C++20 keywords**, not scpp-invented syntax.

### 3.2 Initial implementation scope

I recommend this staged semantic scope:

#### Phase 1 language scope

- free-function coroutines
- `co_return`
- `co_yield`
- `co_await`
- nested `promise_type` on the return type
- member `operator co_await`
- `promise.await_transform(...)`
- awaiters whose `await_suspend` returns `void`, `bool`, or a coroutine handle

#### Deferred from phase 1

- coroutine lambdas
- coroutine member functions
- `std::coroutine_traits` customization beyond the return type's own nested `promise_type`
- free `operator co_await` found via ADL
- external borrows that remain live across suspension
- multi-threaded executor semantics

The restrictions above are not because the broader model is unsound. They are because this is the smallest coherent first language slice that stays C++-shaped, explicit, and compatible with scpp's current no-ADL / ownership / thread-safety principles.

### 3.3 Why not full `std::coroutine_traits` immediately?

Because phase 1 should prefer the most direct and explicit protocol surface:

- `R::promise_type` keeps the coroutine protocol attached to the return type itself
- it avoids introducing global trait indirection as part of the first coroutine surface
- it stays consistent with scpp's general reluctance to rely on ADL-shaped customization paths

So phase 1 should target the **direct nested-promise model** first. Broader `std::coroutine_traits`-style customization can be revisited later if a concrete use case needs it.

## 4. Coroutine semantic model for scpp

## 4.1 Return-type protocol

For a coroutine returning `R`, phase 1 uses:

- `R::promise_type`

The compiler synthesizes the same broad sequence as real C++:

1. create coroutine frame
2. construct promise object in that frame
3. call `promise.get_return_object()` to create the user-visible return value
4. evaluate `promise.initial_suspend()` and run the awaiter protocol
5. execute the body
6. lower each `co_await` / `co_yield` via awaiter protocol
7. lower `co_return expr` to `promise.return_value(expr)` or `co_return;` to `promise.return_void()`
8. evaluate `promise.final_suspend()` and suspend/finalize appropriately
9. destroy frame on `destroy()` / drop-path

## 4.2 `co_await` resolution order

Phase 1 should use this lookup order:

1. if the promise has `await_transform(expr)`, use that result
2. else if the expression type has a member `operator co_await()`, use that result
3. else treat the expression itself as the awaiter

Then require the resulting awaiter to provide:

- `await_ready()`
- `await_suspend(current_coroutine_handle)`
- `await_resume()`

Notes:

- I am intentionally not proposing ADL-found free `operator co_await` in phase 1.
- This matches scpp's existing anti-ADL stance much better.

## 4.3 `co_yield`

`co_yield expr` lowers like real C++:

- call `promise.yield_value(expr)`
- treat the result as an awaitable/awaiter
- run the same awaiter protocol

Empirically, clang's raw pre-split IR does exactly this: each `co_yield` became `yield_value(...)`, then `coro.save`, `coro.await.suspend.*`, `coro.suspend`.

## 4.4 `co_return`

- `co_return expr;` -> `promise.return_value(expr)`
- `co_return;` -> `promise.return_void()`

The compiler should statically enforce the same requirement shape that clang enforces today.

## 4.5 Exceptions / `unhandled_exception`

scpp has no `throw`/`try`/`catch` and its design already commits to value-based recoverable errors.

So there are two plausible designs:

1. **Total-protocol design**: still require `promise_type::unhandled_exception()` to exist, but document it as unreachable in ordinary checked scpp code
2. **scpp-specific simplification**: make it optional/ignored because the language has no ordinary exception path

I recommend **(1) for phase 1**. The reason is not toolchain precedent; it is that scpp should keep one explicit, total coroutine protocol for **normal completion** versus **abnormal escape from the coroutine body**. Ordinary recoverable failures should still travel by value, but if unchecked failure, foreign code, or some future implementation-defined escape path does cross the coroutine boundary, the promise type should name exactly one explicit sink for that path rather than leaving the protocol partial or implicit.

Under that design, `unhandled_exception()` is part of the low-level coroutine contract, while still being unreachable in ordinary checked scpp code. That keeps the protocol explicit and uniform without changing scpp's value-oriented error model. But this is still an open design question (see the end of this document).

## 5. Proposed compiler architecture changes

## 5.1 Lexer/parser/AST

### Lexer

Add tokens for:

- `KwCoAwait`
- `KwCoYield`
- `KwCoReturn`

### AST

Add:

- `ExprKind::CoAwait`
- `ExprKind::CoYield`
- `StmtKind::CoReturn` (or equivalent explicit coroutine-return statement form)
- `Function::is_coroutine`
- coroutine-specific semantic summary fields (promise type, suspend metadata, borrow summary)

Also add nested type declarations in class/struct bodies, because phase-1 promise types need:

```cpp
template<typename T>
struct task {
    struct promise_type { ... };
};
```

Without nested-type support, scpp cannot express idiomatic coroutine return types at all.

## 5.2 MIR extension

The current MIR is good enough structurally (basic blocks + statements + terminators), but it needs explicit coroutine forms.

Suggested additions:

- `MirStatementKind::CoroutineInit`
- `MirStatementKind::Await`
- `MirStatementKind::Yield`
- `MirStatementKind::CoroutineReturn`
- `MirStatementKind::CoroutineSuspendPoint`
- `MirStatementKind::CoroutineDestroyScopeCleanup`

I do **not** think scpp should encode the final LLVM coroutine ABI directly in the parser AST. The right place to make suspend points explicit is MIR.

## 5.3 New analysis: suspension liveness / frame residency

scpp already computes reference liveness for NLL release.

Coroutine support needs one more dataflow analysis:

> for each suspend point, which locals / temporaries / awaiters / promise subobjects are live across it?

That analysis drives three things:

1. which values conceptually become frame-resident
2. which destructors must run on the coroutine destroy path for each suspend state
3. which borrows are allowed vs rejected across suspension

I recommend a dedicated pass after MIR construction and before final coroutine codegen.

## 5.4 Codegen split: ordinary vs coroutine functions

Today `codegen.cppm` lowers ordinary functions directly from AST.

I recommend keeping that for non-coroutines, and adding a second path:

- `define_function(...)` for ordinary functions (existing path)
- `define_coroutine_function(...)` for coroutine functions (new path)

`define_coroutine_function(...)` should emit **pre-split LLVM coroutine IR**, not a hand-written final state machine.

## 6. LLVM mapping design

## 6.1 Core lowering strategy

For a coroutine function, emit a `presplitcoroutine` function using the standard switched-resume intrinsics:

- `llvm.coro.id`
- `llvm.coro.alloc`
- `llvm.coro.size.*`
- allocator call (`new` / runtime allocator)
- `llvm.coro.begin`
- one `llvm.coro.save` + `llvm.coro.suspend` per suspension point
- `llvm.coro.free`
- `llvm.coro.end`
- `llvm.coro.promise` for handle/promise conversions

Then let LLVM's `coro-early` + `coro-split` passes synthesize the real frame, resume, and destroy entrypoints.

## 6.2 Important practical detail

The frontend does **not** need to explicitly materialize the final frame struct layout.

Clang's raw pre-split IR still uses allocas and ordinary temporaries before coroutine passes run; LLVM then rewrites frame-resident state into the coroutine frame.

That is good news for scpp:

- we do not need to invent a second hand-maintained low-level frame layout
- we only need enough frontend analysis to know cleanup and borrow semantics

## 6.3 But scpp still must generate cleanup structure

LLVM can split the state machine, but it does **not** know scpp's ownership semantics by itself.

scpp must still emit the right cleanup/control-flow shape so that:

- destroying a suspended coroutine runs destructors for all currently-live frame-resident objects
- final completion cleans up once
- moved-out values are not destroyed
- unique_ptr-like owned resources are released exactly once

This is the biggest codegen integration point with current `codegen.cppm`, because that file currently assumes:

- stack locals
- lexical `scope_stack_`
- cleanup only at lexical scope exit / ordinary return

Coroutine destroy paths break that assumption.

## 6.4 Recommended coroutine IR shape

For each coroutine function, the frontend should build a coroutine-specific CFG with explicit states:

- entry / promise init
- initial suspend
- resume state N for each suspend point
- final suspend
- destroy path per live-state class
- completion path

This CFG then lowers to pre-split LLVM coroutine IR.

That keeps scpp responsible for:

- promise/awaiter semantics
- destructor placement
- move-state correctness

while leaving LLVM responsible for:

- frame packing
- switch-based resume/destroy function generation
- ABI-level coroutine object layout

## 7. The soundness core: move/borrow/lifetime integration

This is the most important section.

## 7.1 Key distinction: internal vs external borrows

For soundness, scpp must distinguish borrows that cross suspension into two categories.

### A. Internal frame borrows

Example shape:

```cpp
task<int> f() {
    std::string s = ...;
    const std::string& r = s;
    co_await something();
    co_return r.size();
}
```

Here both `s` and `r` are part of the same coroutine state.

This should be **allowed** if:

- both remain live across the suspend point
- the coroutine lowering keeps them in the same stable coroutine frame
- destruction order remains valid

Because the frame address is stable, this is not Rust's movable-future problem.

### B. External borrows

Example shape:

```cpp
task<void> f(int& x) {
    co_await something();
    x = 1;
}
```

Now the coroutine frame keeps a borrow of something **outside itself** across suspension.

This is much harder, because the returned `task<void>` would need to keep `x` borrowed/alive for as long as the coroutine exists, not merely until the borrow's last syntactic use.

That requires new caller-visible lifetime machinery that scpp does not currently have.

## 7.2 Recommended phase-1 rule

**Phase 1 rule:**

> A borrow may remain live across a suspend point only if its root place is itself frame-resident and owned by the coroutine frame.

Corollaries:

- borrowing one coroutine local from another across suspension is allowed
- borrowing a by-value parameter across suspension is allowed **if** that parameter value itself is frame-resident and owned by the coroutine frame, exactly like any other coroutine local
- borrowing a by-reference parameter, caller-owned referent, or outer reference across suspension is rejected in phase 1
- by-reference captures in coroutine lambdas should therefore be deferred with coroutine lambdas themselves

I strongly recommend this boundary.

It fits scpp's current movechecker and avoids inventing half-correct interprocedural lifetime machinery under schedule pressure.

## 7.3 Why current NLL release is insufficient by itself

Current scpp movecheck releases borrows after last use via `compute_reference_liveness(...)` and `release_dead_references(...)`.

That is correct for ordinary locals and for current borrow-holding closures because a closure's reference field destructor is effectively trivial.

A suspended coroutine is different:

- the frame can outlive the current activation
- destroying the coroutine later may run destructors of live frame objects
- those destructors/awaiter objects may still observe borrowed state

So for coroutines, a borrow live across a suspend cannot be modeled as an ordinary intra-activation NLL borrow anymore.

## 7.4 Proposed movecheck extension

Add a coroutine-specific pass over MIR:

1. identify suspension points
2. compute live-across-suspend values
3. classify each live borrow root as internal vs external
4. reject external-across-suspend borrows in phase 1
5. mark internal-across-suspend roots/references as frame-resident

Then adapt the ordinary movecheck rules:

- moving a frame-resident root while a frame-resident borrow of it remains live across a suspend is rejected
- destroying / reassignment of such a root before the borrow ends is rejected
- scope-exit cleanup must use coroutine-state liveness, not only lexical block exit

## 7.5 No user-visible Pin type in phase 1

I do **not** recommend introducing an scpp `Pin<T>` / `Unpin` analogue in phase 1.

Reason:

- Rust needs it because the future object itself is movable state
- scpp's coroutine frame is a separate coroutine object/frame allocation handled by LLVM's switched-resume ABI
- the handle can move while the frame address remains stable

So `Pin` would mostly add surface complexity without solving the real first-order problem.

If scpp later adds a second coroutine model where the state value itself is movable (Rust-style generators/futures as plain values), then revisit this. For the C++ coroutine ABI, it is not the first tool I would add.

## 7.6 What this means for `task<T>` and `generator<T>` soundness

### `generator<T>`

Safe phase-1 profile:

- own state in the frame
- yield owned values (or references to frame-owned state only if carefully designed)
- no external borrow across `co_yield`

### `task<T>`

Safe phase-1 profile:

- lazy task owns its frame
- may await other tasks / awaitables
- may keep internal frame borrows across suspension
- may not keep caller-owned borrows across suspension

That is already useful and implementable.

## 8. Library design

## 8.1 New stdlib partitions

I recommend introducing:

- `std:coroutine`
- `std:task`
- `std:generator`
- `std:executor` (phase 2; see below)

This matches the project's current library organization (`libs/README.md`, `libs/std/`, `libs/scpp/`).

## 8.2 `std:coroutine`

This partition should expose the minimal low-level protocol types:

- `std::coroutine_handle<>`
- `std::coroutine_handle<Promise>`
- `std::suspend_always`
- `std::suspend_never`

Likely operations:

- `from_promise`
- `from_address`
- `address`
- `resume`
- `destroy`
- `done`
- `promise`

**Ownership / safety model:** `std::coroutine_handle<...>` should be a **non-owning, copyable view** of an already-existing coroutine frame. It does **not** destroy the frame on drop, and copying a handle does not duplicate ownership. The owning responsibility for a frame instead lives in higher-level wrappers such as `task<T>`, `generator<T>`, or runtime-internal objects that explicitly manage lifetime.

Under that model:

- `address()`, `from_address(...)`, and `promise()` are low-level escape hatches that should require an explicit unsafe context or otherwise carry explicit caller obligations, because they can fabricate or reinterpret non-owning access to a frame
- `resume()` and `destroy()` are likewise low-level operations that are only sound when the caller proves the handle still denotes a live frame and that the caller currently has the right to resume or destroy that frame
- using any copied handle after the owning `task<T>` / `generator<T>` / runtime owner has destroyed the frame is dangling use and therefore outside the safe subset

This keeps frame ownership single and explicit, avoids double-destroy by construction, and matches scpp's general rule that raw handle-like escape hatches belong at the unsafe boundary rather than masquerading as ordinary owning values.

These low-level protocol carriers should be ordinary `struct` types, not `class` types, unless a future design deliberately needs runtime polymorphism. Under [§11.5](../../spec/en/11-inheritance-and-interfaces.md#115-virtual-destruction-and-explicit-overriding-classdtor-classvirtual), every `class` now requires an explicit virtual destructor, which is the wrong default for coroutine handles and suspend helper values.

These are best treated like a **compiler-backed library surface**, similar in spirit to how scpp already treats some constructs as library-shaped but compiler-known.

## 8.3 `std::generator<T>`

Recommended phase-1 shape:

```cpp
namespace std {
    template<typename T>
    struct generator {
        struct promise_type;

        generator(generator&&);
        ~generator();

        bool next();
        const T& current() const;
        bool done() const;
    };
}
```

Properties:

- move-only
- pull-style consumer API (`next()`)
- no executor needed
- simplest end-to-end first slice for coroutine implementation
- should default to a `struct`, not a `class`, unless a later design intentionally introduces polymorphism

`current() const` should return a reference to the generator's **currently yielded frame-resident value**. That reference remains valid only until the earliest of:

1. the next successful `next()` call on that same generator state (which may resume the coroutine and replace or destroy the previously-yielded value),
2. destruction of the generator, or
3. any operation that destroys the underlying coroutine frame.

A move of the generator transfers ownership of the same underlying frame and therefore need not by itself move the yielded object or invalidate the reference. But after the move, only the moved-to generator may be used to continue or destroy that state; any later `next()` or destruction through the moved-to owner invalidates previously obtained `current()` references in the ordinary way.

## 8.4 `std::task<T>`

Recommended phase-1 shape:

```cpp
namespace std {
    template<typename T>
    struct task {
        struct promise_type;

        task(task&&);
        ~task();

        bool done() const;
        auto operator co_await() &&;
    };
}
```

Important semantic choices:

- **lazy** by default (`initial_suspend = suspend_always` for the standard library task types)
- move-only
- single-consumer
- dropping the task destroys the frame

### Continuation chaining

Use the standard coroutine-handle-returning `await_suspend` form:

- awaiting task A from task B stores B's handle as A's continuation
- A's `final_suspend` returns that continuation handle
- LLVM/ABI-level symmetric transfer does the rest

This is the cleanest way to get task-to-task composition without building a heavy scheduler first.

## 8.5 Do we need an executor immediately?

For **language bring-up and tests**, the truly minimal layer is:

- `generator<T>`
- `task<T>`
- `std::sync_wait(task<T>&&)`

That is enough to prove:

- suspend/resume/destroy
- task awaiting task
- promise protocol
- cleanup correctness

For **real async I/O usability**, yes, an executor/event loop is needed.

So my recommendation is:

### phase 1 runtime minimum

- `generator<T>`
- `task<T>`
- `sync_wait`

### phase 2 runtime minimum

- explicit single-threaded executor with ready queue
- scheduling awaitables (`yield_now`, maybe `schedule_on(executor&)`)
- later: reactor-backed awaitables that can integrate with the httpserver runtime work

## 8.6 Executor design recommendation

Because scpp currently lacks atomics/mutex/condition-variable/channel/container infrastructure in stdlib, phase 2 should start with an **explicit single-threaded executor**.

Reasons:

- matches current language/library maturity
- avoids immediate interaction with `[[scpp::thread_movable]]` resume-on-other-thread semantics
- can plausibly share conceptual structure with `applications/httpserver/runtime/httpserver_runtime.scpp` later
- does not require hidden globals (which scpp does not support today anyway)

Suggested minimal API:

```cpp
namespace std {
    struct single_thread_executor {
        void spawn(task<void>&& t);
        void run();
    };
}
```

with awaitables like:

- `std::yield_now()`
- later, reactor-backed socket/file/event awaitables

## 8.7 Thread-safety traits and coroutines

This is intentionally conservative for phase 1.

I recommend:

- do **not** promise that `task<T>` / `generator<T>` are thread-movable in phase 1
- keep the first executor single-threaded
- revisit thread traits only once cross-thread resume semantics are designed

If a later executor/scheduler abstraction is expressed as an interface rather than a concrete `struct`, the coroutine design must also respect current interface rules: interface-typed pointers/references are two-word representations under [§11.3](../../spec/en/11-inheritance-and-interfaces.md#113-base-specifiers-and-interface-identity-classmi), and parameter-level `[[scpp::thread_movable]]` / `[[scpp::thread_shareable]]` checks on interface-typed parameters constrain the concrete implementor type under [§8.5](../../spec/en/04-thread-safety-properties.md#85-interface-contracts-and-interface-typed-parameters-dclattrscppthreadinterface). Also, because `docs/open_issues.md` still notes that structural derivation currently ignores inherited ordinary base subobjects, phase-1 coroutine wrapper types should avoid ordinary inheritance and prefer direct data members.

Longer-term, the right rule is probably:

- a coroutine handle type is thread-movable iff its hidden frame state is thread-movable and the scheduling contract permits cross-thread resume
- a coroutine handle is thread-shareable only with much stronger guarantees

But that needs explicit design, not an implicit guess.

## 9. Proposed phased implementation plan

## Phase A — prerequisites for expressing coroutine libraries

1. add lexer/parser support for `co_await`, `co_yield`, `co_return`
2. add AST nodes
3. add nested type declarations in class/struct bodies (`promise_type`)
4. add compiler-backed `std:coroutine` primitives
5. add tests for parser/AST only

## Phase B — generator-first end-to-end slice

1. extend MIR with suspend points
2. add suspension-liveness/frame-residency analysis
3. add movecheck rule: reject external-across-suspend borrows
4. add coroutine codegen path emitting pre-split LLVM IR
5. add `std::generator<T>`
6. validate with:
   - `co_yield`
   - generator destruction while suspended
   - owned local cleanup
   - internal local-to-local borrow across `co_yield`

Why generator first:

- no task continuation chaining yet
- no executor required
- exercises the same core frame/destroy machinery

## Phase C — task and `co_await`

1. implement awaiter resolution (`await_transform`, member `operator co_await`, direct awaiter)
2. support `await_suspend` returning `void` / `bool` / handle
3. add `std::task<T>`
4. add `sync_wait`
5. validate task-awaits-task continuation chaining

## Phase D — executor

1. single-threaded ready-queue executor
2. `yield_now` / schedule primitives
3. integration tests with nested tasks and explicit scheduling

## Phase E — optional later expansions

- coroutine lambdas
- coroutine member functions
- free `operator co_await`
- `std::coroutine_traits` customization
- external-across-suspend borrow support
- thread-movable task/generator + multi-thread executor
- reactor/I/O awaitables, potentially sharing concepts with the httpserver runtime

## 10. Capability-gap ledger

| Area | Current scpp status | Needed for coroutines | Consequence |
|---|---|---|---|
| Coroutine tokens | absent | add `co_await/co_yield/co_return` | parser cannot recognize coroutine syntax today |
| Coroutine AST nodes | absent | explicit coroutine expression/statement nodes | movecheck/codegen have nowhere to attach semantics |
| Nested promise type | absent in class/struct model | nested `promise_type` declarations | idiomatic coroutine return types cannot be expressed |
| Coroutine MIR | absent | suspend-point-aware MIR | movecheck cannot reason about suspension |
| Suspend liveness analysis | absent | live-across-suspend analysis | no sound frame-residency / cleanup decision |
| Borrow summary across suspension | absent | at least internal-vs-external classification | phase-1 soundness boundary cannot be enforced |
| Coroutine codegen path | absent | pre-split LLVM coroutine IR emission | current AST codegen assumes ordinary stack function only |
| Destroy-path cleanup | lexical only | per-suspend-state cleanup | suspended coroutine destruction would leak or double-destroy |
| `std:coroutine` primitives | absent | coroutine_handle / suspend types | user/library promise types cannot be written |
| Executor/runtime layer | absent | at least `sync_wait`, later executor | `task` coroutines cannot be driven usefully |
| ADL-based await customization | globally rejected by project | explicit design choice required | free `operator co_await` cannot simply be copied from full C++ |
| Threaded scheduling story | not designed | executor/thread-trait rules | cross-thread resume would be underspecified |

## 11. Recommended design decisions to settle now

These are the decisions I recommend the implementation follow unless the user prefers otherwise.

1. **Use LLVM switched-resume coroutines, not a hand-written state machine.**
2. **Do not add a user-visible Pin type in phase 1.**
3. **Make phase 1 lazy and generator/task-focused.**
4. **Reject external borrows across suspension in phase 1.**
5. **Start with free-function coroutines only.**
6. **Support nested `promise_type`, `await_transform`, and member `operator co_await`; defer ADL/free-function customization.**
7. **Bring up `generator<T>` before `task<T>`.**
8. **Bring up `sync_wait` before a full executor.**
9. **Keep the first executor single-threaded.**

## 12. Open design questions for user review

These are the places where I think there is a real fork rather than a purely mechanical implementation choice.

1. **How strict should phase 1 be about external borrows across suspension?**
   My recommendation: reject them entirely in phase 1.

2. **Should scpp require `promise_type::unhandled_exception()` even though scpp has no exceptions?**
   My recommendation: yes for protocol parity, but document it as unreachable in checked scpp.

3. **Should phase 1 support only direct nested `R::promise_type`, or also `std::coroutine_traits` customization?**
   My recommendation: direct nested `promise_type` only in phase 1.

4. **Should phase 1 support free `operator co_await` despite the language's no-ADL stance?**
   My recommendation: no; support `await_transform` + member `operator co_await` only.

5. **Should coroutine lambdas and coroutine methods be in the initial implementation slice, or explicitly deferred?**
   My recommendation: defer them until the core free-function pipeline is stable.

6. **Should `task<T>` be lazy or eager?**
   My recommendation: lazy.

7. **How much executor/runtime is part of the first deliverable?**
   My recommendation: `generator<T>`, `task<T>`, and `sync_wait` first; explicit single-threaded executor second.

8. **Should `task<T>` / `generator<T>` be thread-movable in phase 1?**
   My recommendation: no promise yet; leave multi-thread resume for a later design round.

## Appendix A — empirical findings worth preserving

### LLVM

- local LLVM 22 intrinsic definitions were confirmed in `/usr/lib/llvm-22/include/llvm/IR/Intrinsics.td`
- local hand-written `minimal-coro.ll` successfully lowered with:

```sh
opt-22 -passes='coro-early,coro-split' minimal-coro.ll -S
```

and produced a frame type plus `resume`, `destroy`, and `cleanup` functions

### Clang

- raw pre-pass coroutine IR was observed with:

```sh
clang++-22 -std=c++20 -O0 -S -emit-llvm -Xclang -disable-llvm-passes ...
```

- that IR contained `llvm.coro.id/alloc/begin/save/suspend/free/end/promise`
- a `co_yield` example showed repeated `yield_value(...)` + suspend lowering
- runtime logging examples confirmed:
  - `await_transform`
  - `operator co_await`
  - `await_suspend` return-type variants (`void`, `bool`, handle)

### Rust

- `Future::poll(&mut fut, ...)` fails because `poll` requires `Pin<&mut _>`
- `async fn` futures are `!Unpin`
- moving a manually self-referential struct changed the address of `data` while an internal pointer still pointed to the old address
- `Pin::into_inner` was rejected for a `PhantomPinned` type

Those findings are the main reason this document recommends **no Pin-equivalent for internal coroutine-frame borrows**, but **a strict phase-1 ban on external-across-suspend borrows**.
