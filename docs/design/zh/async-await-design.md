# SCPP async/await / 协程设计

状态：已按当前 `main` 刷新；仍为实现前评审用的设计草案

## 0. 范围与核心结论

本文档提出 scpp 应如何加入 **C++20/23 风格的协程**：`co_await`、`co_yield`、`co_return`、promise-type 协议、协程句柄，以及一个最小化的库层（`task`、`generator`、执行器辅助设施）。

本设计首先立足于 scpp 已经定下来的语言原则与当前规范文本，尤其是：

- 默认优先采用 C++ 风格的表层语法与语义；只有在 scpp 的重大缺口确实需要时才新增规则
- 显式优先于隐式，包括 scpp 一贯不依赖 ADL 形状定制点的立场
- 线程安全作为一等机制同时通过结构化推导与显式契约表达（`docs/spec/en/04-thread-safety-properties.md`，尤其是 8.5）
- 当前的继承 / interface 模型，包括 `class` 的强制虚析构规则，以及 interface 表示的双 machine-word 语义（`docs/spec/en/11-inheritance-and-interfaces.md`）

这里会把 C++、Rust 以及其他支持协程的语言当作参考输入与对比样本，但它们不会凌驾于 scpp 自身的设计目标之上；当前编译器实现状态也只被视为可行性/实现分期的输入，而不是判断语言设计是否成立的标准。

## 0.1 与当前文档/规范布局的关系

- `docs/missings.md` 目前仍记录着：scpp 还没有 async/await、future 或 stream 的完整方案。
- `docs/open_issues.md` 目前没有 async 专属阻塞项；但其中关于“线程安全结构化推导尚未计入继承来的 ordinary base 子对象”的说明，会影响任何试图用 ordinary inheritance 实现协程包装类型的方案。
- `docs/spec/en/README.md` 目前还没有协程条款。未来如果把本设计正式写进规范，不应硬编码过时的未来章节号，而应按届时最新的规范结构来安放。

## 1. 可行性小结

### 1.1 已确认

1. **LLVM 已经具备正确的 lowering 基础设施。**
   本地 LLVM 22 在 `/usr/lib/llvm-22/include/llvm/IR/Intrinsics.td` 中暴露了标准的 switched-resume 协程 intrinsic：
   `llvm.coro.id`、`alloc`、`size`、`begin`、`save`、`suspend`、`free`、`end`、`resume`、`destroy`、`promise`。

2. **`opt-22 -passes=coro-early,coro-split` 确实能把一个未拆分的协程拆分成 ramp/resume/destroy 函数。**
   本地手写的 `minimal-coro.ll` 使用 `llvm.coro.*` lower 出了一个 frame 类型，加上 `f.resume`、`f.destroy` 和 `f.cleanup` 函数。所以 scpp **不需要**自己手动搭建最终的状态机。

3. **Clang 内部仍然使用这些 intrinsic。**
   用 `clang++-22 -Xclang -disable-llvm-passes -S -emit-llvm`，一个简单的协程会生成包含 `llvm.coro.id/alloc/begin/save/suspend/free/end/promise` 的 `presplitcoroutine` IR。不加 `-disable-llvm-passes` 时，clang 输出的是已经拆分好的状态机，这与 clang 在最终 IR 生成之前先运行协程 lowering 的事实一致。

4. **真实的 C++ promise/awaiter 语义表现符合预期。**
   本地实验确认：
   - `co_return expr` 需要 `promise_type::return_value(expr-type)`
   - `co_return;` 需要 `promise_type::return_void()`
   - `co_yield expr` 需要 `promise_type::yield_value(expr-type)`
   - promise 各钩子按预期顺序被调用：`get_return_object`、`initial_suspend`、函数体、`return_*`、`final_suspend`
   - `co_await` 若存在 `promise.await_transform(...)`，会优先查询它
   - awaiter 协议是 `await_ready`、`await_suspend`、`await_resume`
   - `await_suspend` 可以返回 `void`、`bool`，或 `std::coroutine_handle<>`

5. **Rust 印证了核心的健全性隐患，但也澄清了 scpp 所选 ABI 有一个重要的不同之处。**
   本地 `rustc` 实验确认：
   - `Future::poll` 需要 `Pin<&mut Self>`
   - `async fn` 的 future 是 `!Unpin`
   - 移动一个手工构造的自引用值会使其内部自指针失效
   - 对 `PhantomPinned` 值调用 `Pin::into_inner` 会被拒绝

### 1.2 关于帧稳定性的最重要结论

Rust 在这里是有价值的对照，但不是基线设计目标。

Rust 需要 `Pin`，是因为 **future 值本身就是状态对象**，所以移动 future 可能移动自引用状态。

而对于 scpp 拟采用的 **C++ 协程模型**，源码层面的协程返回的是一个 **指向单独分配的协程帧的句柄**（即 LLVM/clang 的 switched-resume ABI）。移动用户可见的 `task<T>` / `generator<T>` 对象时，只移动句柄，**不**移动帧本身。

**因此：scpp 大概率不需要为协程帧内部的自借用（self-borrow）引入用户可见的 `Pin` 等价物。**

这是本次设计里最大的一项简化。

### 1.3 主要的不确定性

剩下的难点**不是**帧自引用问题，而是：

> 如何健全地处理**跨越挂起点仍然存活的、指向帧外数据的借用**。

本文档给出一个保守的第一阶段（phase 1）答案：

- 当引用对象和引用本身都位于协程帧内时，**允许跨挂起的内部借用**
- 第一阶段**拒绝跨挂起的外部借用**
- 如果以后确有需要，再重新考虑支持携带借用的协程返回对象

我认为这是 scpp 当前 movechecker 的正确第一个实现边界。

## 2. 立足于 scpp 当前实现模型（非规范性）

本节讨论的是实现落点与分期，不是用来判断该语言设计在语义上是否成立。


目前相关的管线是：

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

来自真实代码库的具体依据：

- `src/lexer.cppm` 目前没有协程相关的 token。
- `src/ast.cppm` 目前没有协程 AST 节点；语句种类止步于 `VarDecl/Return/If/While/Break/Continue/ExprStmt/Block`，表达式种类止步于现有的运行时/泛型/lambda 形式。
- `src/mir.cppm` 只为 movecheck 构建 CFG MIR；目前完全没有挂起点的概念。
- `src/movecheck.cppm` 是函数内（intraprocedural）且 NLL 风格的：它为引用局部变量计算 `live_after` 集合，并在最后一次使用之后释放借用。
- `src/codegen.cppm` 仍然直接从 AST lower 普通函数，使用词法作用域栈 + 显式析构函数发射。
- `src/driver.cppm` 依次运行 `monomorphize_generics(program)`、`check_moves(program)`，然后是 codegen。

由此可知：

- parser/AST/MIR/movecheck 都需要具备协程感知能力
- codegen 不可能完全保持不变，因为协程 lowering 需要 suspend/resume/destroy 路径
- 但最终的帧拆分/状态机重写仍可以交给 LLVM 完成

## 3. 拟议的表层语言设计

### 3.1 语法

原样采用真实的 C++ 协程拼写：

- `co_await expr`
- `co_yield expr`
- `co_return expr;`
- `co_return;`

这与项目既定的设计哲学一致：这些是**既有的 C++20 关键字**，不是 scpp 自创的语法。

### 3.2 初期实现范围

我建议采用如下分阶段的语义范围：

#### 第一阶段语言范围

- 自由函数（free-function）协程
- `co_return`
- `co_yield`
- `co_await`
- 返回类型上嵌套的 `promise_type`
- 成员 `operator co_await`
- `promise.await_transform(...)`
- `await_suspend` 返回 `void`、`bool`，或协程句柄的 awaiter

#### 推迟到第一阶段之后

- 协程 lambda
- 协程成员函数
- 超出返回类型自身嵌套 `promise_type` 之外的 `std::coroutine_traits` 定制
- 通过 ADL 找到的自由 `operator co_await`
- 跨挂起仍然存活的外部借用
- 多线程执行器语义

以上限制并不是因为更宽的模型不健全；而是因为这是最小、连贯的第一阶段语言切片，同时又保持了 C++ 风格、显式优先，以及 scpp 当前的无 ADL / 所有权 / 线程安全原则。

### 3.3 为什么不立即支持完整的 `std::coroutine_traits`？

因为第一阶段应优先选择最直接、最显式的协议表面：

- `R::promise_type` 让协程协议直接附着在返回类型本身上
- 它避免把全局 trait 间接层一并引入第一版协程表面
- 它也更符合 scpp 一贯不依赖 ADL 形状定制点的立场

所以第一阶段应当先针对**直接嵌套 promise 模型**。更宽的 `std::coroutine_traits` 式定制可以等到将来有具体用例时再重新评估。

## 4. scpp 的协程语义模型

## 4.1 返回类型协议

对于返回 `R` 的协程，第一阶段使用：

- `R::promise_type`

编译器合成与真实 C++ 大体相同的序列：

1. 创建协程帧
2. 在该帧中构造 promise 对象
3. 调用 `promise.get_return_object()` 创建用户可见的返回值
4. 求值 `promise.initial_suspend()` 并运行 awaiter 协议
5. 执行函数体
6. 通过 awaiter 协议 lower 每一个 `co_await` / `co_yield`
7. 将 `co_return expr` lower 为 `promise.return_value(expr)`，将 `co_return;` lower 为 `promise.return_void()`
8. 求值 `promise.final_suspend()`，并相应地挂起/收尾
9. 在 `destroy()` / drop 路径上销毁帧

## 4.2 `co_await` 的解析顺序

第一阶段应使用如下查找顺序：

1. 若 promise 具有 `await_transform(expr)`，使用其结果
2. 否则若表达式类型具有成员 `operator co_await()`，使用其结果
3. 否则将表达式本身当作 awaiter

然后要求得到的 awaiter 提供：

- `await_ready()`
- `await_suspend(current_coroutine_handle)`
- `await_resume()`

说明：

- 我有意不在第一阶段提议通过 ADL 找到的自由 `operator co_await`。
- 这与 scpp 既有的反 ADL 立场更加吻合。

## 4.3 `co_yield`

`co_yield expr` 与真实 C++ 一样进行 lower：

- 调用 `promise.yield_value(expr)`
- 将结果当作一个 awaitable/awaiter
- 运行相同的 awaiter 协议

实证上，clang 未拆分的原始 IR 正是这样做的：每个 `co_yield` 变成 `yield_value(...)`，然后是 `coro.save`、`coro.await.suspend.*`、`coro.suspend`。

## 4.4 `co_return`

- `co_return expr;` -> `promise.return_value(expr)`
- `co_return;` -> `promise.return_void()`

编译器应静态地强制执行与今天 clang 相同的这套要求形态。

## 4.5 异常 / `unhandled_exception`

scpp 没有 `throw`/`try`/`catch`，其设计已经承诺采用基于值的可恢复错误处理方式。

因此有两种可行设计：

1. **完整协议设计**：仍然要求 `promise_type::unhandled_exception()` 存在，但将其文档化为在正常受检查的 scpp 代码中不可达
2. **scpp 专属简化**：因为语言没有通常意义上的异常路径，使其可选/被忽略

我推荐**第一阶段采用方案 (1)**。理由不是工具链先例，而是：scpp 应该为协程保留一套明确且完整的低层协议，用来区分**正常完成**与**协程体异常退出**。普通可恢复失败仍然应按值传播；但如果某种未受检查的失败、foreign code，或未来某种实现定义的逃逸路径真的跨越了协程边界，promise type 就应当明确命名唯一的承接钩子，而不是让协议变成不完整或隐式。

在这种设计下，`unhandled_exception()` 是低层协程契约的一部分，同时在普通受检查的 scpp 代码中仍然不可达。这样既保持协议显式且统一，又不改变 scpp 以值为中心的错误模型。但这仍是一个开放的设计问题（见本文档末尾）。

## 5. 拟议的编译器架构变化

## 5.1 Lexer/parser/AST

### Lexer

新增 token：

- `KwCoAwait`
- `KwCoYield`
- `KwCoReturn`

### AST

新增：

- `ExprKind::CoAwait`
- `ExprKind::CoYield`
- `StmtKind::CoReturn`（或等价的显式协程返回语句形式）
- `Function::is_coroutine`
- 协程专属的语义摘要字段（promise 类型、挂起元数据、借用摘要）

同时新增类体内的嵌套类型声明，因为第一阶段的 promise 类型需要：

```cpp
template<typename T>
struct task {
    struct promise_type { ... };
};
```

没有嵌套类型支持，scpp 就完全无法表达符合惯用法的协程返回类型。

## 5.2 MIR 扩展

当前的 MIR 在结构上已经足够（基本块 + 语句 + 终结指令），但需要显式的协程形式。

建议新增：

- `MirStatementKind::CoroutineInit`
- `MirStatementKind::Await`
- `MirStatementKind::Yield`
- `MirStatementKind::CoroutineReturn`
- `MirStatementKind::CoroutineSuspendPoint`
- `MirStatementKind::CoroutineDestroyScopeCleanup`

我**不**认为 scpp 应该把最终的 LLVM 协程 ABI 直接编码进 parser 的 AST 里。让挂起点显式化的正确位置是 MIR。

## 5.3 新增分析：挂起活跃性 / 帧驻留性

scpp 已经为 NLL 释放计算引用活跃性。

协程支持还需要一项额外的数据流分析：

> 对每个挂起点而言，哪些局部变量/临时对象/awaiter/promise 子对象跨越它仍然存活？

这项分析驱动三件事：

1. 哪些值在概念上会变成帧驻留的
2. 每个挂起状态下，协程销毁路径必须运行哪些析构函数
3. 跨挂起哪些借用是允许的、哪些是被拒绝的

我建议在 MIR 构建之后、最终协程 codegen 之前，增加一趟专门的 pass。

## 5.4 Codegen 拆分：普通函数 vs 协程函数

目前 `codegen.cppm` 直接从 AST lower 普通函数。

我建议对非协程函数保留这一做法，并新增第二条路径：

- `define_function(...)` 用于普通函数（既有路径）
- `define_coroutine_function(...)` 用于协程函数（新路径）

`define_coroutine_function(...)` 应当发射**未拆分的 LLVM 协程 IR**，而不是手写的最终状态机。

## 6. LLVM 映射设计

## 6.1 核心 lowering 策略

对于一个协程函数，使用标准的 switched-resume intrinsic 发射一个 `presplitcoroutine` 函数：

- `llvm.coro.id`
- `llvm.coro.alloc`
- `llvm.coro.size.*`
- 分配器调用（`new` / 运行时分配器）
- `llvm.coro.begin`
- 每个挂起点一对 `llvm.coro.save` + `llvm.coro.suspend`
- `llvm.coro.free`
- `llvm.coro.end`
- `llvm.coro.promise` 用于句柄/promise 之间的转换

然后让 LLVM 的 `coro-early` + `coro-split` pass 去合成真正的 frame、resume 和 destroy 入口函数。

## 6.2 一个重要的实践细节

前端**不需要**显式地把最终的 frame 结构体布局具体化。

Clang 未拆分的原始 IR 在协程 pass 运行之前仍然使用 alloca 和普通临时变量；随后 LLVM 会把帧驻留状态重写进协程帧。

这对 scpp 是个好消息：

- 我们不需要发明第二套手工维护的底层 frame 布局
- 我们只需要足够的前端分析来知晓清理和借用语义

## 6.3 但 scpp 仍必须生成清理结构

LLVM 可以拆分状态机，但它本身并不知道 scpp 的所有权语义。

scpp 仍必须发射正确的清理/控制流形态，以确保：

- 销毁一个挂起中的协程时，会为当前所有存活的帧驻留对象运行析构函数
- 正常完成时只清理一次
- 已经被移出的值不会被析构
- 类似 unique_ptr 的自有资源恰好被释放一次

这是与当前 `codegen.cppm` 集成时最大的 codegen 难点，因为该文件目前假定：

- 栈上局部变量
- 词法 `scope_stack_`
- 只在词法作用域退出/普通 return 时才清理

协程的销毁路径打破了这个假设。

## 6.4 推荐的协程 IR 形态

对每个协程函数，前端应构建一个协程专属的、带有显式状态的 CFG：

- 入口 / promise 初始化
- 初始挂起（initial suspend）
- 每个挂起点对应一个恢复状态 N
- 最终挂起（final suspend）
- 每一类存活状态各自的销毁路径
- 正常完成路径

这个 CFG 随后 lower 为未拆分的 LLVM 协程 IR。

这样 scpp 依然负责：

- promise/awaiter 语义
- 析构函数的放置
- move 状态的正确性

而把以下事情交给 LLVM 负责：

- 帧打包
- 基于 switch 的 resume/destroy 函数生成
- ABI 层面的协程对象布局

## 7. 健全性核心：move/borrow/lifetime 集成

这是最重要的一节。

## 7.1 关键区分：内部借用 vs 外部借用

出于健全性考虑，scpp 必须把跨越挂起的借用分成两类。

### A. 内部帧借用

示例形态：

```cpp
task<int> f() {
    std::string s = ...;
    const std::string& r = s;
    co_await something();
    co_return r.size();
}
```

这里 `s` 和 `r` 都是同一个协程状态的一部分。

只要满足以下条件，就应当**允许**：

- 二者都跨越挂起点存活
- 协程 lowering 把它们保留在同一个稳定的协程帧中
- 析构顺序依然合法

因为帧地址是稳定的，所以这不是 Rust 那种可移动 future 的问题。

### B. 外部借用

示例形态：

```cpp
task<void> f(int& x) {
    co_await something();
    x = 1;
}
```

现在协程帧跨越挂起持有了一个指向**自身之外**的借用。

这困难得多，因为返回的 `task<void>` 需要让 `x` 在整个协程存续期间保持被借用/存活，而不仅仅是到该借用最后一次语法上被使用为止。

这需要一套 scpp 目前还不具备的、对调用方可见的新生命周期机制。

## 7.2 建议的第一阶段规则

**第一阶段规则：**

> 一个借用只有在其根位置（root place）本身是帧驻留的、并由协程帧所拥有时，才可以跨越挂起点存活。

推论：

- 跨挂起从一个协程局部变量借用另一个协程局部变量是允许的
- 跨挂起借用**按值参数**是允许的，只要该参数值本身是帧驻留的并由协程帧拥有；这一点与普通协程局部变量相同
- 第一阶段拒绝跨挂起借用按引用参数、调用方拥有的 referent，或外部引用
- 因此协程 lambda 中的按引用捕获应当与协程 lambda 本身一起推迟

我强烈建议采用这条边界。

它符合 scpp 目前的 movechecker，并避免在进度压力下发明一套半正确的跨过程（interprocedural）生命周期机制。

## 7.3 为什么现有的 NLL 释放机制本身不够用

当前 scpp 的 movecheck 通过 `compute_reference_liveness(...)` 和 `release_dead_references(...)`，在最后一次使用之后释放借用。

这对普通局部变量、以及当前持有借用的闭包来说是正确的，因为闭包的引用字段析构函数实际上是平凡的。

而挂起中的协程不同：

- 帧的生存期可以超出当前这次激活（activation）
- 之后销毁协程可能会运行存活帧对象的析构函数
- 这些析构函数/awaiter 对象仍可能观察到被借用的状态

所以对协程而言，跨挂起存活的借用不能再被建模为一个普通的、单次激活内的 NLL 借用。

## 7.4 拟议的 movecheck 扩展

在 MIR 上新增一趟协程专属的 pass：

1. 识别挂起点
2. 计算跨挂起存活的值
3. 将每个跨挂起存活的借用根分类为内部或外部
4. 第一阶段拒绝跨挂起的外部借用
5. 将内部跨挂起存活的根/引用标记为帧驻留

然后调整普通的 movecheck 规则：

- 当一个帧驻留根的帧驻留借用跨挂起仍然存活时，移动该根会被拒绝
- 在该借用结束之前销毁/重新赋值这样的根会被拒绝
- 作用域退出时的清理必须使用协程状态活跃性，而不仅仅是词法块退出

## 7.5 第一阶段不引入用户可见的 Pin 类型

我**不**建议在第一阶段引入 scpp 版的 `Pin<T>` / `Unpin` 类似物。

原因：

- Rust 需要它，是因为 future 对象本身就是可移动的状态
- scpp 的协程帧是由 LLVM switched-resume ABI 处理的、单独的协程对象/帧分配
- 句柄可以移动，而帧地址保持稳定

所以 `Pin` 大概率只会增加表层复杂度，却解决不了真正的一阶问题。

如果 scpp 以后引入第二种协程模型，其中状态值本身就是可移动的（Rust 风格的、作为普通值的 generator/future），那时再重新考虑这个问题。对于 C++ 协程 ABI 来说，它不是我会优先加入的工具。

## 7.6 这对 `task<T>` 和 `generator<T>` 健全性意味着什么

### `generator<T>`

第一阶段的安全画像：

- 状态归帧所有
- 产出（yield）自有值（或者仅在精心设计的情况下产出指向帧自有状态的引用）
- 没有跨 `co_yield` 的外部借用

### `task<T>`

第一阶段的安全画像：

- 惰性 task 拥有自己的帧
- 可以 await 其他 task/awaitable
- 可以跨挂起保留内部帧借用
- 不可以跨挂起保留调用方拥有的借用

这已经足够有用，也足够可实现。

## 8. 库设计

## 8.1 新的标准库分区

我建议引入：

- `std:coroutine`
- `std:task`
- `std:generator`
- `std:executor`（第二阶段；见下文）

这与项目当前的库组织方式一致（`libs/README.md`、`libs/std/`、`libs/scpp/`）。

## 8.2 `std:coroutine`

该分区应暴露最小化的底层协议类型：

- `std::coroutine_handle<>`
- `std::coroutine_handle<Promise>`
- `std::suspend_always`
- `std::suspend_never`

可能的操作：

- `from_promise`
- `from_address`
- `address`
- `resume`
- `destroy`
- `done`
- `promise`

**所有权 / 安全模型：**`std::coroutine_handle<...>` 应当是一个**非 owning、可拷贝的视图**，指向某个已经存在的协程帧。它在析构/离开作用域时**不会**销毁该帧，复制句柄也不会复制所有权。帧的 owning 责任应当放在更高层包装物上，例如 `task<T>`、`generator<T>`，或某些显式管理生命周期的运行时内部对象。

在这个模型下：

- `address()`、`from_address(...)` 和 `promise()` 都是底层 escape hatch，应要求显式 unsafe context，或至少带有明确的调用者义务，因为它们能够伪造或重解释对协程帧的非 owning 访问
- `resume()` 和 `destroy()` 同样是底层操作；只有当调用者能证明该句柄仍然指向一个活着的帧，并且调用者当前确实拥有恢复或销毁该帧的权限时，它们才是健全的
- 在 owning 的 `task<T>` / `generator<T>` / 运行时 owner 销毁该帧之后，继续使用任何先前复制出来的句柄都会形成悬垂使用，因此不属于 safe subset

这样可以从构造上避免 double-destroy，保持帧所有权单一且显式，也符合 scpp 的一般原则：这类原始 handle 式 escape hatch 应位于 unsafe 边界，而不应伪装成普通 owning 值。

这些底层协议承载体应默认采用普通 `struct`，而不是 `class`；除非未来某个设计明确需要运行时多态。按照 [§11.5](../../spec/en/11-inheritance-and-interfaces.md#115-virtual-destruction-and-explicit-overriding-classdtor-classvirtual)，现在每个 `class` 都必须显式声明虚析构函数，这对协程句柄和 suspend 辅助值来说并不是合适的默认值。

这些最好被当作一种**编译器支持的库表层**，在精神上类似于 scpp 已经把某些构造视为“库形态，但编译器已知”的做法。

## 8.3 `std::generator<T>`

推荐的第一阶段形态：

```cpp
namespace std {
    template<typename T>
    struct generator {
    public:
        struct promise_type;

        generator(generator&&);
        ~generator();

        bool next();
        const T& current() const;
        bool done() const;
    };
}
```

特性：

- move-only
- 拉取式（pull-style）消费者 API（`next()`）
- 不需要执行器
- 是协程实现最简单的端到端首个切片
- 默认应使用 `struct`，而不是 `class`；除非后续设计有意引入多态

`current() const` 应返回对 generator **当前已产出、且驻留在帧中的值**的引用。该引用只在以下事件中最先发生之前保持有效：

1. 对同一 generator 状态下一次成功的 `next()` 调用（它可能恢复协程，并替换或销毁先前产出的值），
2. generator 被销毁，或
3. 任何会销毁底层协程帧的操作发生。

移动 generator 会转移同一个底层帧的所有权，因此**移动本身**不必移动已产出的对象，也不必立即使该引用失效。但在 move 之后，只允许 moved-to 的 generator 继续推进或销毁该状态；之后通过 moved-to owner 进行的 `next()` 或析构，会按通常方式使先前取得的 `current()` 引用失效。

## 8.4 `std::task<T>`

推荐的第一阶段形态：

```cpp
namespace std {
    template<typename T>
    struct task {
    public:
        struct promise_type;

        task(task&&);
        ~task();

        bool done() const;
        auto operator co_await() &&;
    };
}
```

重要的语义选择：

- 标准库 task 类型默认为**惰性（lazy）**（`initial_suspend = suspend_always`）
- move-only
- 单一消费者
- 丢弃 task 会销毁帧

### 延续（continuation）链接

使用标准的、返回协程句柄的 `await_suspend` 形式：

- task B await task A 时，把 B 的句柄存为 A 的延续
- A 的 `final_suspend` 返回该延续句柄
- LLVM/ABI 层面的对称转移（symmetric transfer）负责剩下的工作

这是在不先构建一个庞大调度器的前提下，实现 task 到 task 组合的最干净方式。

## 8.5 我们立即就需要一个执行器吗？

对于**语言的初期落地与测试**而言，真正最小化的层是：

- `generator<T>`
- `task<T>`
- `std::sync_wait(task<T>&&)`

这已经足够证明：

- suspend/resume/destroy
- task await task
- promise 协议
- 清理正确性

对于**真实可用的异步 I/O**而言，确实需要一个执行器/事件循环。

所以我的建议是：

### 第一阶段运行时最小集

- `generator<T>`
- `task<T>`
- `sync_wait`

### 第二阶段运行时最小集

- 带就绪队列的显式单线程执行器
- 调度用的 awaitable（`yield_now`，也许还有 `schedule_on(executor&)`）
- 之后：可与 httpserver 运行时相关工作集成的、基于 reactor 的 awaitable

## 8.6 执行器设计建议

因为 scpp 目前在标准库中还缺少原子/互斥量/条件变量/channel/容器基础设施，第二阶段应当从一个**显式的单线程执行器**开始。

理由：

- 与当前语言/库的成熟度相匹配
- 避免立即与 `[[scpp::thread_movable]]` 那种"在其他线程上恢复"的语义发生交互
- 之后可以合理地与 `applications/httpserver/runtime/httpserver_runtime.scpp` 共享概念结构
- 不需要隐藏的全局变量（反正 scpp 目前也不支持）

建议的最小 API：

```cpp
namespace std {
    struct single_thread_executor {
    public:
        void spawn(task<void>&& t);
        void run();
    };
}
```

配合如下 awaitable：

- `std::yield_now()`
- 之后，基于 reactor 的 socket/file/事件 awaitable

## 8.7 线程安全 trait 与协程

第一阶段在这方面刻意保守。

我建议：

- 第一阶段**不**承诺 `task<T>` / `generator<T>` 是 thread-movable 的
- 首个执行器保持单线程
- 只有在设计好跨线程恢复语义之后，才重新考虑线程 trait

如果以后把 executor / scheduler 抽象成 interface，而不是具体 `struct`，协程设计还必须同时遵守当前 interface 规则：interface 类型的指针/引用在 [§11.3](../../spec/en/11-inheritance-and-interfaces.md#113-base-specifiers-and-interface-identity-classmi) 下是双 machine-word 表示；而 interface 类型参数上的 `[[scpp::thread_movable]]` / `[[scpp::thread_shareable]]` 检查，会按 [§8.5](../../spec/en/04-thread-safety-properties.md#85-interface-contracts-and-interface-typed-parameters-dclattrscppthreadinterface) 约束具体实现者类型，而不只是 interface 声明本身。另外，由于 `docs/open_issues.md` 仍指出结构化推导当前不会计入继承来的 ordinary base 子对象，第一阶段的协程包装类型应避免 ordinary inheritance，优先使用直接数据成员。

长远来看，正确的规则大概是：

- 当且仅当一个协程句柄的隐藏帧状态是 thread-movable 的、且调度契约允许跨线程恢复时，该协程句柄类型才是 thread-movable 的
- 只有在有更强保证的情况下，协程句柄才是 thread-shareable 的

但这需要显式设计，而不是隐式猜测。

## 9. 拟议的分阶段实现计划

## 阶段 A —— 表达协程库所需的前置条件

1. 为 `co_await`、`co_yield`、`co_return` 添加 lexer/parser 支持
2. 添加 AST 节点
3. 在 class/struct 体内添加嵌套类型声明（`promise_type`）
4. 添加编译器支持的 `std:coroutine` 原语
5. 只针对 parser/AST 添加测试

## 阶段 B —— 以 generator 优先的端到端切片

1. 用挂起点扩展 MIR
2. 添加挂起活跃性/帧驻留性分析
3. 添加 movecheck 规则：拒绝跨挂起的外部借用
4. 添加发射未拆分 LLVM IR 的协程 codegen 路径
5. 添加 `std::generator<T>`
6. 通过以下场景验证：
   - `co_yield`
   - 在挂起状态下销毁 generator
   - 自有局部变量的清理
   - 跨 `co_yield` 的内部局部变量到局部变量借用

为什么先做 generator：

- 还不涉及 task 的延续链接
- 不需要执行器
- 会锻炼相同的核心 frame/destroy 机制

## 阶段 C —— task 与 `co_await`

1. 实现 awaiter 解析（`await_transform`、成员 `operator co_await`、直接 awaiter）
2. 支持 `await_suspend` 返回 `void` / `bool` / 句柄
3. 添加 `std::task<T>`
4. 添加 `sync_wait`
5. 验证 task await task 的延续链接

## 阶段 D —— 执行器

1. 单线程就绪队列执行器
2. `yield_now` / 调度原语
3. 针对嵌套 task 和显式调度的集成测试

## 阶段 E —— 可选的后续扩展

- 协程 lambda
- 协程成员函数
- 自由 `operator co_await`
- `std::coroutine_traits` 定制
- 跨挂起外部借用支持
- thread-movable 的 task/generator + 多线程执行器
- reactor/I/O awaitable，可能与 httpserver 运行时共享概念

## 10. 能力缺口台账

| 领域 | scpp 当前状态 | 协程所需 | 后果 |
|---|---|---|---|
| 协程 token | 缺失 | 添加 `co_await/co_yield/co_return` | parser 目前无法识别协程语法 |
| 协程 AST 节点 | 缺失 | 显式的协程表达式/语句节点 | movecheck/codegen 无处挂接语义 |
| 嵌套 promise 类型 | class/struct 模型中缺失 | 嵌套的 `promise_type` 声明 | 无法表达符合惯用法的协程返回类型 |
| 协程 MIR | 缺失 | 具备挂起点感知的 MIR | movecheck 无法推理挂起 |
| 挂起活跃性分析 | 缺失 | 跨挂起存活分析 | 无法健全地决定帧驻留性/清理方式 |
| 跨挂起借用摘要 | 缺失 | 至少需要内部/外部分类 | 无法强制执行第一阶段的健全性边界 |
| 协程 codegen 路径 | 缺失 | 发射未拆分的 LLVM 协程 IR | 现有 AST codegen 只假定普通栈函数 |
| 销毁路径清理 | 仅限词法 | 按挂起状态清理 | 挂起中的协程销毁会泄漏或重复析构 |
| `std:coroutine` 原语 | 缺失 | coroutine_handle / suspend 类型 | 用户/库无法编写 promise 类型 |
| 执行器/运行时层 | 缺失 | 至少需要 `sync_wait`，之后需要执行器 | `task` 协程无法被有效驱动 |
| 基于 ADL 的 await 定制 | 项目全局拒绝 | 需要明确的设计选择 | 自由 `operator co_await` 无法直接照搬完整 C++ |
| 多线程调度方案 | 尚未设计 | 执行器/线程 trait 规则 | 跨线程恢复将处于欠规范状态 |

## 11. 建议现在就敲定的设计决策

除非用户另有偏好，以下是我建议实现遵循的决策。

1. **使用 LLVM 的 switched-resume 协程，而不是手写状态机。**
2. **第一阶段不添加用户可见的 Pin 类型。**
3. **让第一阶段是惰性的，并以 generator/task 为重点。**
4. **第一阶段拒绝跨挂起的外部借用。**
5. **只从自由函数协程开始。**
6. **支持嵌套 `promise_type`、`await_transform` 和成员 `operator co_await`；推迟 ADL/自由函数定制。**
7. **在 `task<T>` 之前先落地 `generator<T>`。**
8. **在完整执行器之前先落地 `sync_wait`。**
9. **让首个执行器保持单线程。**

## 12. 供用户评审的开放设计问题

以下是我认为存在真正分歧、而非纯机械实现选择的地方。

1. **第一阶段对跨挂起外部借用应当有多严格？**
   我的建议：第一阶段完全拒绝。

2. **尽管 scpp 没有异常，是否仍应要求 `promise_type::unhandled_exception()`？**
   我的建议：为了协议对等性应当要求，但在受检查的 scpp 中将其文档化为不可达。

3. **第一阶段是仅支持直接嵌套的 `R::promise_type`，还是也支持 `std::coroutine_traits` 定制？**
   我的建议：第一阶段仅支持直接嵌套的 `promise_type`。

4. **尽管语言坚持无 ADL 立场，第一阶段是否应支持自由 `operator co_await`？**
   我的建议：不支持；只支持 `await_transform` + 成员 `operator co_await`。

5. **协程 lambda 和协程方法应纳入首个实现切片，还是明确推迟？**
   我的建议：推迟，直到核心的自由函数管线稳定为止。

6. **`task<T>` 应该是惰性的还是及早（eager）求值的？**
   我的建议：惰性。

7. **首个交付物中应包含多少执行器/运行时内容？**
   我的建议：先做 `generator<T>`、`task<T>` 和 `sync_wait`；显式的单线程执行器排在第二位。

8. **第一阶段 `task<T>` / `generator<T>` 是否应为 thread-movable？**
   我的建议：暂不作此承诺；把多线程恢复留给以后的设计轮次。

## 附录 A —— 值得保留的实证发现

### LLVM

- 本地 LLVM 22 的 intrinsic 定义已在 `/usr/lib/llvm-22/include/llvm/IR/Intrinsics.td` 中确认
- 本地手写的 `minimal-coro.ll` 用以下命令成功完成 lowering：

```sh
opt-22 -passes='coro-early,coro-split' minimal-coro.ll -S
```

并生成了一个 frame 类型，加上 `resume`、`destroy` 和 `cleanup` 函数

### Clang

- 使用以下命令观察到了未经 pass 处理的原始协程 IR：

```sh
clang++-22 -std=c++20 -O0 -S -emit-llvm -Xclang -disable-llvm-passes ...
```

- 该 IR 包含 `llvm.coro.id/alloc/begin/save/suspend/free/end/promise`
- 一个 `co_yield` 示例显示了反复出现的 `yield_value(...)` + 挂起 lowering
- 运行时日志示例确认了：
  - `await_transform`
  - `operator co_await`
  - `await_suspend` 的返回类型变体（`void`、`bool`、句柄）

### Rust

- `Future::poll(&mut fut, ...)` 失败，因为 `poll` 需要 `Pin<&mut _>`
- `async fn` 的 future 是 `!Unpin`
- 移动一个手工构造的自引用 struct 改变了 `data` 的地址，而内部指针仍然指向旧地址
- 对 `PhantomPinned` 类型调用 `Pin::into_inner` 被拒绝

这些发现是本文档建议**协程帧内部借用不设 Pin 等价物**、但**对跨挂起外部借用第一阶段严格禁止**的主要原因。
