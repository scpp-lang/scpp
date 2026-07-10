# SCPP constexpr / consteval 引擎设计

状态：研究完成后的首轮评审草案，尚未实现

## 0. 范围与结论

本文提出 scpp 应如何加入一个**通用的编译期求值引擎**，支持真实 C++ 风格的 `constexpr` / `consteval`，并以最小但可落地的架构先解决当前最紧迫的标准库缺口：

- 真实、带类型的 `std::format_string<Args...>` / `std::print` / `std::println`；以及
- 像 C++20/C++23 `<format>` 一样，在编译期拒绝格式串与参数不匹配的程序。[^format-string][^cpp-consteval]

核心建议是：

1. 保持 scpp 的**真实 C++ 拼写原则**：直接复用 `constexpr`、`consteval`、`if consteval`、`std::is_constant_evaluated()`，不发明新语法；[^spec-front-matter][^old-book-consteval]
2. 用**前端解释器**实现编译期求值，而不是降到 LLVM IR 之后再执行，更不是运行宿主机代码；[^clang-users][^gcc-constexpr][^rust-dev-guide]
3. 把该解释器接到**泛型调用单态化 + 语义检查**阶段，而不是接到 codegen；
4. 明确给出一个**诚实、可文档化的 v1 子集**，但这个子集已经足以覆盖格式串验证、常量表达式初始化、递归辅助函数、以及直接的编译期解析器。

一句话：**前端 AST 解释器 + 与 C++ 对齐的表面语义 + scpp 风格的明确 v1 子集 + 直接解锁 `std::format_string<Args...>`。**

## 1. scpp 当前起点

### 1.1 现有前端流水线

当前编译器流程仍然很直接：

1. 解析为 AST；
2. 对泛型调用做单态化；
3. 做 move/dataflow 检查；
4. 生成 LLVM IR。[^driver-pipeline]

因此 constexpr/consteval 最合适的接入点，就是**解析之后、codegen 之前、并且已经拿到完整类型信息的前端阶段**。

### 1.2 现有泛型机制“差一点”，但还不够

现有泛型调用路径已经能处理：

- `T x` 这类直接参数位置上的普通类型推导；以及
- tuple 风格递归继承所需的那一个特殊“基类链推导”模式。[^movecheck-deduction][^movecheck-base-deduction][^old-book-generic-types]

但当前实现本质上仍然是**单趟、从左到右**的。它只会在下面两种位置绑定模板参数包：

- 直接的尾部函数参数包；或
- 已有的 tuple 风格基类推导模式。[^movecheck-deduction]

这就无法处理 `<format>` 的关键形状：

```cpp
template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args);
```

这里真正需要的是：

1. 从**后面的** `args...` 实参绑定 `Args...`；
2. 再把这个参数包代入到**前面的** `format_string<Args...>` 里；
3. 然后再拿第一个实参去检查这个已经具体化的参数类型。

当前单态化器做不到第 2 步，因为它走过一个参数时就想当场把它定下来。[^movecheck-deduction]

### 1.3 现有 AST / parser 还没有 constexpr/consteval 模型

当前 lexer 没有 `KwConstexpr` / `KwConsteval`，AST 的 `Function` 也没有“求值模式”字段，语句解析里也没有 `if consteval`。[^lexer-keywords][^ast-function]

此外，旧版书还明确解释过：scpp 早期只保留 `consteval`，刻意不支持 `constexpr` 函数，因为它的行为会依赖调用上下文。[^old-book-consteval]

这段历史很重要，但同一段文字也留下了一个回旋空间：如果以后真的出现“同一个函数既要服务编译期又要服务运行期”的强需求，那么应该重新引入 `constexpr`。`std::format_string<Args...>` 正是这个需求。[^old-book-consteval]

### 1.4 现有“编译期求值”只是一小块局部逻辑

编译器今天已经做过一件极小范围的编译期求值：可变泛型类型的非类型模板参数（整数常量、参数名、和 `+`）。源码自己就明确说了：这**不是通用 consteval 机制**。[^movecheck-non-type]

这段代码仍然很有价值，因为它证明了：**在前端里做一个小型、受控的解释器逻辑，本来就是这个代码库可接受的做法**。新的 constexpr 引擎应该是在这个方向上推广，而不是把问题推给 LLVM。

### 1.5 现有模块产物也需要扩展

现在的 `.scppm` 只会为“导出的泛型定义”额外保留源代码体（`SGEN` 块），而导入 `.scppm` 时，编译器又明确**完全不读取这个块**。[^driver-sgen][^driver-scppm-read]

这对 imported `constexpr` / `consteval` 代码显然不够，而且从模块长期架构看也不正确。如果用户导入了 `std::format_string`，那么导入方必须能够在语义检查阶段执行它的导出构造函数体，以及它在同一模块里调用到的私有 helper。本文最终定稿的方向因此是：编译期相关定义应通过 `.scppm` 里的**结构化 AST 序列化**跨模块传递，而不是继续依赖源代码快照后续再解析。

## 2. 来自成熟语言/编译器的研究结论

## 2.1 应该复用的真实 C++ 表面语义

scpp 应该直接复用的 C++ 规则其实很清晰：

- `consteval` 声明**立即函数**（immediate function）：每一个 potentially-evaluated 的调用都必须产出常量表达式，否则程序不合法。[^cpp-consteval]
- `constexpr` 声明一个**可以**在常量表达式上下文中编译期执行、但也可以在运行期正常调用的函数。[^cpp-constexpr]
- `if consteval` 根据当前是否处于 manifestly constant-evaluated context 选择分支；真分支还是 immediate-function context。[^cpp-if]
- `std::is_constant_evaluated()` 暴露同一个判定。[^cpp-is-constant]
- C++ 的常量表达式在编译期会拒绝很多 UB 形态：调用非 `constexpr` 函数、溢出、除零、越界读、超过实现限制等等。[^cpp-constant-expression]

对 scpp 来说最关键的是这条分界：

- **`consteval` 失败永远是调用点上的编译错误。**
- **`constexpr` 只有在外层上下文“要求常量表达式”时，失败才是编译错误。** 否则它仍然可以作为普通运行期调用存在。

正是这条分界，才能让 `std::format_string<Args...>` 可行，而不把所有格式化 helper 都强行变成“只能编译期调用”。

## 2.2 真实编译器内部是怎么做 constexpr 的

主流编译器的实现模式同样很明确：

- Clang 长期使用前端里的 AST 解释器 `ExprConstant.cpp`，维护自己的求值上下文、虚拟调用栈，以及深度/步数限制。[^clang-users][^clang-exprconstant]
- GCC 的 `cp/constexpr.cc` 也是前端求值，并显式提供 `-fconstexpr-depth`、`-fconstexpr-loop-limit`、`-fconstexpr-ops-limit` 这类限制。[^gcc-constexpr]

Clang 现在也有了一个较新的字节码常量解释器，但那是 Clang 内部的进一步演化，不是 scpp 起步时最需要复制的最小模式。[^clang-constant-interpreter]

对 scpp 的架构启示就是：

- **不要**先编成机器码再执行；
- **不要**等到 LLVM IR 都出来以后才求值；
- **要**在前端里解释，并且由编译器自己管理内存、限制、诊断。

这和 scpp 现有架构明显更契合。

## 2.3 值得比较的相邻语言设计

### Rust

Rust 的 const-eval 比正常执行更受限制，编译器通过 **MIR 上的虚拟机** 来做编译期解释，而不是运行宿主机代码。[^rust-reference][^rust-dev-guide][^rust-interpreter]

Rust 给 scpp 的两个重要启发是：

1. **编译期求值必须遵循 target model，而不是 host model**；[^rust-reference]
2. 即便不是 AST，而是中间表示，核心抽象仍然是“解释器”。

scpp 应该立刻采纳第 1 点；但在 v1 里没必要为了 constexpr 专门引入 MIR，保留 AST 级实现更合适。

### Zig

Zig 的 `comptime` 明显更激进：任意代码都可以被强制放到编译期执行，compile-time-known value 深度参与类型系统，同一套语言表面也承担元编程职责。[^zig-comptime]

它的优点是野心足够大：它证明了“编译期执行真正有用”的前提，是它可以跑普通控制流和普通 helper。

它和 scpp 冲突的地方在于语法和哲学：

- Zig 是显式的 `comptime` 模型；
- scpp 的设计底线是**除非真实 C++ 表面完全表达不了，否则不新增语法**。[^spec-front-matter]

所以 scpp 应借鉴的是“普通代码、普通控制流也能编译期执行”的方向，而不是 Zig 的表面机制。

### D

D 的 CTFE（Compile Time Function Execution）在精神上更接近 scpp：普通函数在“需要常量”的上下文里可以于编译期执行，但支持的子集由实现明确界定。[^d-ctfe]

scpp 最该借鉴的不是 D 的精确规则，而是它的产品策略：**先交付一个清楚、可用、文档化的子集，本身就是合理路线**，只要对不支持的情况失败得足够明确。

## 3. scpp 应遵守的设计原则

综合研究结果和当前架构，constexpr 设计应遵守以下原则。

1. **完全复用标准 C++ 拼写。**
   scpp 接受 `constexpr`、`consteval`、`if consteval`、`std::is_constant_evaluated()`，并尽量对齐真实 C++ 语义。[^cpp-consteval][^cpp-if][^cpp-is-constant]

2. **求值必须留在前端。**
   解释器直接操作 scpp AST 和解析后的类型信息，在 LLVM codegen 之前结束。

3. **v1 子集必须明确写清楚。**
   和 scpp 其他特性一样，constexpr 支持应该诚实说明“现在支持什么、明确不支持什么”。

4. **遵循 target 语义，而不是 host 语义。**
   整数位宽、指针大小、溢出行为等都必须跟随目标三元组，而不是跟随编译器当前运行的宿主机。[^rust-reference]

5. **只要解释器“看得见”的编译期 UB，就升级为语义错误。**
   当编译期求值是强制的，只要解释器能检测到溢出、除零、越界、生命周期失效或非法 unsafe，就直接报错。

6. **v1 直接禁止 `[[scpp::unsafe]]` 进入常量求值。**
   第一版不尝试提供“无检查的编译期执行”；一旦在 required constant evaluation 里碰到 unsafe block / unsafe function，就编译错误。

7. **保留 scpp 现有产物分层。**
   `.scppm` 仍是编译期边界，`.scppa` 仍是原生代码边界；但 `.scppm` 必须开始携带导出的 constexpr/consteval/generic 函数体。

## 4. 顶层设计建议

## 4.1 表面语法与 AST 扩展

### Lexer / parser

新增关键字：

- `constexpr`
- `consteval`

并扩展 `if` 解析，支持：

- `if consteval { ... } else { ... }`
- `if !consteval { ... } else { ... }`

### AST

为 `Function` 增加求值模式枚举：

```cpp
enum class FunctionEvalMode {
    RuntimeOnly,
    Constexpr,
    Consteval,
};
```

同时增加：

- 变量声明上的 `bool is_constexpr`；
- `Stmt` 上的 `IfMode`（`RuntimeIf` / `ConstexprIf` / `ConstevalIf` / `NotConstevalIf`），而不是额外发明一种新的 statement kind。

这样对 AST 的改动最小，也能最大化复用现有 `StmtKind::If` 处理路径。[^ast-function][^ast-stmt]

## 4.2 v1 支持子集

### v1 明确支持

第一版引擎应支持以下对象在编译期求值：

- 标量字面量与标量局部变量：
  - `bool`、`char`、`int`、`long`，以及前端已经建模过的 unsigned 整数；
  - `float`、`double`，以及项目现有前端 / codegen 已建模的浮点标量族；
- 字符串字面量：内部表示为只读静态字节数组，但对现有 scpp 类型系统仍表现为 `const char*`；[^movecheck-string][^codegen-string]
- 空指针、以及指向字符串字面量 / constexpr 全局对象静态存储区内部的指针；
- 元素类型可 constexpr 的定长数组；
- 由数组或字符串字面量派生出的只读 `std::span<const T>`；
- 字段本身均可 constexpr 的 trivial `struct`；
- 满足下列条件的一部分 `class`：
  - 所有字段都可 constexpr；
  - 构造通过 `constexpr` 或 `consteval` 构造函数完成；
  - v1 不需要执行析构中的用户代码；
  - 不含 `mutable` 字段；
  - 不使用 unsafe；
- 控制流：
  - block
  - 局部变量声明
  - 赋值
  - `if`
  - `while`
  - `return`
  - 递归
- 对 `constexpr` / `consteval` 函数的普通调用；
- 在检查型常量表达式上下文中支持浮点算术与比较；对解释器可见的非法操作（例如除零）在编译期直接拒绝，整体方向与普通 constant-expression failure 保持一致；[^cpp-constant-expression]
- 对已经完成单态化后的 fold / pack-expansion helper 代码做编译期执行。

这已经足以覆盖：

- 格式串扫描与占位符解析；
- 对字符串字面量做边界检查后的逐字符遍历；
- 基于递归的编译期 helper；
- `format_string<Args...>("{}")` 这类 immediate constructor。

### v1 明确不支持

在第一版中，required constant evaluation 遇到以下情况应直接失败：

- `[[scpp::unsafe]]` block 或 unsafe function；
- `extern "C"` 调用、模块外函数体不可见的 module-extern 调用、以及所有 FFI；
- `new`、`delete`、堆分配、smart pointer 分配 helper、以及任何动态生命周期管理；
- union 读写；
- 通过原始指针做可变写入；
- 虚调用或任何运行期动态类型机制；
- lambda（等闭包对象与 capture 生命周期模型在解释器里落定后再说）；
- 异常 / throw；
- I/O、线程、同步、环境访问、文件系统访问；
- 需要执行用户自定义析构逻辑的编译期对象；
- 超过解释器预算的循环或递归。

### 预算限制

参考 Clang/GCC 的做法，v1 直接给出硬限制：[^clang-users][^gcc-constexpr]

- 最大调用深度：**512**
- 最大总步数：**1,000,000**
- 单个循环最大迭代次数：**262,144**

v1 先写成编译器内部常量，不急着暴露为 CLI 参数。

## 4.3 参数包推导修复：让 `format_string<Args...>` 成立

### 当前失败点

现在的 `monomorphize_generic_function_call` 是边走参数边决定绑定。[^movecheck-deduction]

这对下面几种形状足够：

- `template<typename T> void f(T x)`
- `template<typename... Args> void f(Args&&... args)`
- tuple 风格基类推导

但它无法处理：

```cpp
template<typename... Args>
void print(format_string<Args...> fmt, Args&&... args);
```

因为第 0 个参数依赖一个只有在第 1 个参数那里才绑定出来的 pack。

### 替换算法：三阶段求解

把 full-header generic call 的推导改成三阶段。

#### 阶段 A：先灌入显式模板实参

和现在一样：

- 先绑定显式的非 pack 类型实参；
- 先绑定显式的非类型实参；
- 如果显式给了 pack，也先收集 pack 元素。

#### 阶段 B：扫描参数/实参，生成约束

把“当场绑定或失败”的逻辑替换成“先收集约束”。

对每个 parameter/argument 对，生成下列约束之一：

1. **直接类型绑定**
   - 模式 `T`
   - 绑定 `T := arg_type`

2. **直接非类型绑定**
   - 继续复用现有整数模板实参逻辑

3. **直接函数参数包绑定**
   - 模式 `Args&&... args`
   - 绑定 `Args... := [arg_type_0, arg_type_1, ...]`

4. **基类链推导绑定**
   - 保留现有 tuple-like 逻辑，作为一种特化约束生成器

5. **延后兼容性检查义务（deferred compatibility obligation）**
   - 参数类型依赖模板参数或模板参数包，但这个参数本身**不是**直接推导源
   - 记录 `(param_index, parameter_type_pattern, argument_expr)`，稍后再检查

`format_string<Args...>` 就是第 5 种。

#### 阶段 C：先把绑定解完，再回头检查延后义务

整次调用扫描完成后：

1. 确认所有非 pack 模板参数都已绑定；
2. 定稿 pack 绑定；
3. 把最终绑定代入每个延后参数类型；
4. 用“普通参数兼容性检查”重新检查这组延后 pair。

于是 `format_string<Args...>` 会被具体化为：

```cpp
format_string<int, const char*>
```

然后才去检查第一个实参是否能初始化这个具体类型——也就是 `consteval` 构造函数真正触发的地方。

### 与现有代码的具体衔接

这不是推倒重写，而是沿着当前已有代码路径做扩展：

1. 保留 `parse_param_list` 的现有规则：真正的函数参数包在语法上仍必须位于参数列表最后。[^parser-pack]
2. 保留 `deduce_via_base_class_chain`，把它作为一种专门的约束生成器。[^movecheck-base-deduction]
3. 把当前 `monomorphize_generic_function_call` 的主体替换为：
   - `collect_template_constraints(...)`
   - `solve_template_constraints(...)`
   - `check_deferred_template_obligations(...)`
4. 增加一个缺失的辅助函数：

```cpp
Type substitute_type_pack(const Type& pattern,
                          std::string_view pack_name,
                          const std::vector<Type>& pack_elems);
```

这就是“前面参数依赖后面才绑定出的 pack”所真正缺的基础设施。AST 里其实已经有 `Type::template_args` 和 `is_pack_expansion` 这种专门表示 symbolic pack reference 的哨兵位；只是现在从未在“任意早先参数类型”里做过这类代换。[^ast-pack-expansion]

### 这个修复为什么边界刚好

这个修复足以解决 `format_string<Args...>`，但**不会**把 scpp 拉进任意复杂的 C++ 模板推导泥潭。

它仍然刻意**不**做：

- 无关模板重载之间的 partial ordering；
- 在函数体内部“拆头递归剩余参数”的任意模板推导；
- 从任意用户定义转换里推导模板参数。

它只补上了一件缺的能力：**先绑定，后代入，再检查。**

## 4.4 求值器架构

## 4.4.1 核心选择

**结论：v1 采用前端内的 AST 树遍历解释器。**

被否决的方案：

- **LLVM IR 解释/JIT** —— 进入流水线太晚、容易混入 host 环境、诊断很差、也不适合语义阶段失败；
- **为了 constexpr 单独引入一套 MIR/bytecode** —— 以后可能值得，但第一版基础设施开销太大；
- **按特性零碎地各写一个“求值器”** —— 只会把当前 non-type-template-arg 的“一次性逻辑”问题放大。

## 4.4.2 模块落点

新增一个前端模块，例如 `src/constexpr.cppm`，导出：

- `ConstValue`
- `ConstexprError`
- `ConstexprEngine`
- 以及少量辅助 API，例如 `evaluate_required_constant_expression(...)`、`evaluate_immediate_call(...)`

该引擎由以下阶段调用：

- 泛型调用单态化；
- movecheck / 语义检查；
- 模块接口加载（决定哪些函数体必须保留）；
- 未来 `constexpr` 变量、数组边界等常量表达式位置。

也就是说，它是一个**语义检查阶段的新子系统**，而不是一个独立后端 pass。

## 4.4.3 解释器内部运行时模型

解释器内部需要四样东西。

### 1. 编译期值表示

使用标签联合：

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

载荷使用 target-aware 表示。

### 2. 虚拟存储

内存统一由编译器自管：

- 全局量 / 字符串字面量：不可变静态存储；
- 每个函数调用一帧；
- 每个局部变量 / 参数一个 slot；
- 字段访问和数组元素访问对应子对象寻址。

指针不是宿主机地址，而是 `(storage_id, offset, pointee_type)` 这种指向虚拟存储的引用。

### 3. 求值上下文

记录：

- 当前模式（`RuntimeCheckOnly` / `RequiredConstexpr` / `ImmediateConsteval`）
- 调用栈帧
- 步数计数器
- 循环迭代计数器
- 递归深度
- 当前源位置（用于诊断）

### 4. 结果缓存

缓存这些场景的成功求值结果：

- constexpr 变量初始化；
- 对字符串字面量的 immediate constructor 调用；
- 格式串校验中反复调用的单态化 helper。

缓存 key 应至少包含：`(函数名, 具体实参值/类型, target triple)`。

## 4.4.4 对模块接口格式的影响

当前 `.scppm` 的附加能力仍然只服务泛型，而且导入时完全不读。[^driver-sgen][^driver-scppm-read]

这里的最终定稿设计，是从一开始就改为**结构化 AST 序列化**，不再沿着“源码快照”方向扩展。

### 结构化 AST 载荷

`.scppm` 应增加一个结构化的“编译期 AST”区段，里面存放的是序列化后的 AST 节点，而不是源代码文本。这个区段应至少携带：

1. 导出的 `constexpr` / `consteval` 定义；
2. 导出的 generic 定义；
3. 从这些导出的编译期相关定义可达的私有 helper 定义、类型定义、常量初始化器；
4. 足以让导入方在不重新解析源代码的前提下，把这些节点重新挂接回语义环境的符号 / 类型元数据。

### 为什么现在就做结构化 AST

这条路更激进，但它是正确的 v1 路线。

- 它更符合成熟模块实现的收敛方向：导入模板与可 constexpr 求值实体，应该以编译器可直接消费的结构化前端状态存在，而不是以原始源码快照形式让每个 importer 再解析一遍。
- 它避免把“头文件时代、每个消费方都重新 parse 一次源码”的模型，再次固化进 scpp 的模块系统；而 scpp 正是在加入真正的跨模块编译期执行能力。
- 它让 generics 与 constexpr body 从一开始就共用同一类长期表示边界。

### 关于“并存还是统一”的建议

本文明确建议**统一，不建议长期并存**：

- 新的结构化序列化机制应同时承担 constexpr-evaluable body 与 scpp 现有 exported generic body 的跨模块表示；
- 当前 `SGEN` 源码快照路径应被视为 reference compiler 的过渡实现细节，而不是第二套永久模块接口机制。

原因是架构层面的，而不是样式层面的。Imported generic monomorphization 与 imported constexpr evaluation，本质上都是“跨模块消费前端 body graph”。如果为 generics 保留一套 source-snapshot 路径、为 constexpr 再引入一套 structured-AST 路径，那么可达性规则、版本化、反序列化、语义重新挂接逻辑都会被重复维护两次，但它们承载的其实是同一种 payload。

## 4.5 诊断与错误分类

### 建议新增 `ConstexprError`

当前的 `ParseError` / `DataflowError` / `CodegenError` / `DriverError` 与现有流水线的分层很一致。[^parser-error][^movecheck-error][^codegen-error][^driver-error]

而编译期求值失败足够特殊，值得单独设一种：

```cpp
struct ConstexprError : std::runtime_error {
    SourceLocation loc;
    std::vector<ConstexprFrame> stack;
};
```

为什么不用 `DataflowError`：

- 这类错误可能发生在“类型本身没错，但这里不能编译期执行”的代码上；
- 用户会很需要 constexpr stack trace（例如“在求值 `parse_replacement_field` 时，由谁调用到这里”）；
- 它能把 move/borrow 失败和“编译期执行失败”清晰区分开。

现有 CLI / project 诊断打印器已经会打印 `SourceLocation`，只要多加一个 catch 分支，再支持输出 note 即可。[^cli-diagnostic]

### v1 至少要报的错误

当常量求值是强制的，下面这些情况应直接报硬错误：

- 调用了非 `constexpr` 函数；
- 在非 immediate context 调用了 `consteval`；
- 检查模式下的整数溢出；
- 除零 / 模零；
- 数组 / span / 字符串越界；
- 读取未初始化虚拟 slot；
- 进入 unsafe 路径；
- 超出步数 / 深度 / 循环限制；
- 需要求值的 imported constexpr body 不可用。

真实 C++ 某些角落会写“ill-formed, no diagnostic required”；但只要 scpp 的解释器**能看见问题**，就完全可以选择报诊断。这和 Clause 4 的总体“至少给出一个诊断”的方向，以及 scpp 一贯的安全优先设计并不冲突。[^spec-front-matter][^cpp-constexpr][^cpp-constant-expression]

## 4.6 scpp 中 `constexpr` 与 `consteval` 的语义分工

### `consteval`

- 拼写就是 `consteval`
- 每个 potentially-evaluated 调用都是 immediate
- 被选中的重载必须能在编译期完成求值
- 一旦失败，就是调用点上的编译错误
- 编译期求值时可以再调用别的 `consteval` 或 `constexpr` 函数
- 和 C++ 一样，不能用于析构函数、分配函数、释放函数[^cpp-consteval]

### `constexpr`

- 拼写就是 `constexpr`
- 函数“具有编译期可执行资格”
- 若外层上下文要求常量表达式，则必须成功求值
- 否则就仍然是普通运行期调用
- 在重载决议里按普通函数处理，不赋予额外偏好

### `if consteval`

- parser 明确识别，AST 中明确标注
- 编译期解释执行时，根据当前求值模式走对应分支
- 真分支创建 immediate-function context，对齐 C++23[^cpp-if]

### `std::is_constant_evaluated()`

v1 可把它当作“编译器已知的标准库 intrinsic”：

- 编译期解释执行时：返回 `true`
- 普通运行期 codegen 时：返回 `false`

这已经足够覆盖标准库使用场景，而且也符合 C++ 实现中“往往借助 `if consteval` 或编译器 builtin”来实现它的现实。[^cpp-is-constant]

## 5. Worked example：`std::format_string<Args...>`

一个对 scpp 友好的 v1 标准库草图可以长这样：

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

现在看下面这段用户代码：

```cpp
int x = 1;
const char* name = "scpp";
std::print("{} {}", x, name);
```

在本文设计下，编译器应这样处理：

1. parser 正常得到 full-header generic `print` 模板；
2. generic-call monomorphization 扫描这次调用；
3. 第 1 个参数（`Args&&... args`）把 `Args...` 绑定为 `[int, const char*]`；
4. 第 0 个参数被记为一个 deferred compatibility obligation，类型模式是 `format_string<Args...>`；
5. 在参数绑定定稿后，前一个参数的具体类型就变成 `format_string<int, const char*>`；
6. 第一个实参是字符串字面量，在今天的 scpp 类型模型里它的类型仍表现为 `const char*`；[^movecheck-string][^codegen-string]
7. 普通参数兼容性检查发现：它要初始化 `format_string<int, const char*>`，只能通过构造函数；
8. 因为这个构造函数是 `consteval`，所以编译器调用 `ConstexprEngine::evaluate_immediate_call`；
9. 在这个 immediate call 里，`detail::validate_format<int, const char*>(s)` 以编译期解释执行的方式运行；
10. 如果格式串里正好有两个自动编号占位符，而参数包长度也是 2，则求值成功，整个调用合法。

如果用户写成：

```cpp
std::print("{} {} {}", x, name);
```

那么第 9 步会抛出 `ConstexprError`，主诊断落在调用点，同时带出虚拟 constexpr 栈上的 note，例如：

```text
main.scpp:12:11: error: consteval construction of 'std::format_string<int, const char*>' failed
note: while evaluating 'std::detail::validate_format<int, const char*>'
note: format string expects 3 arguments, but call supplies 2
```

这就把当前 motivating gap 完整地闭合了。

## 6. 分阶段实现计划

## Phase A — 语法、AST 与序列化模式

1. 为 `constexpr` / `consteval` 增加 lexer token
2. 增加 `FunctionEvalMode`、变量 `is_constexpr`、以及 `if consteval` 的 AST 标记
3. 解析带这些说明符的函数 / 构造函数声明
4. 定义一个带版本号的结构化 `.scppm` AST 载荷格式，用于承载编译期相关定义
5. 标记 exported generic / `constexpr` / `consteval` 根节点，并计算必须随之序列化的可达 helper/type 图

可独立合并 / 测试：

- parser 测试
- AST 序列化模式测试
- 导出编译期定义的可达性测试
- 暂不需要解释器

## Phase B — 结构化 `.scppm` 序列化 / 反序列化

1. 对 exported generic 与 constexpr 相关定义序列化真实 AST 节点，而不是源码快照
2. 导入时反序列化这些 AST 节点，并在 importing compiler session 中重新挂接符号 / 类型身份
3. 用结构化表示取代当前 generics-only 的 source snapshot 路径
4. 对不携带所需结构化载荷的旧 `.scppm` 文件给出清晰拒绝诊断
5. 增加聚焦的跨模块测试，覆盖 imported generic body 与 imported `consteval` helper

可独立合并 / 测试：

- module-interface round-trip 测试
- imported generic body 测试
- imported constexpr body 可用性测试

## Phase C — 参数包推导重构

1. 把 full-header generic-call monomorphization 重构为“收集约束 + 求解”
2. 加入 deferred compatibility obligation
3. 增加 `substitute_type_pack(...)`，支持更早参数类型中的 pack 代换
4. 保留 tuple/base-class deduction 作为专门约束生成器
5. 增加聚焦测试：
   - `template<typename... Args> void f(F<Args...>, Args&&...);`
   - “显式模板实参 + 推导模板实参”混合情形
   - 先代换后发现前面参数不兼容时的错误诊断

可独立合并 / 测试：

- 还不依赖 constexpr 引擎
- 单独这一阶段就应该让 `format_string<Args...>` 这种参数形状“能被推导到位”

## Phase D — 最小可用 constexpr / consteval 引擎

1. 加入 `ConstexprError`、`ConstValue`、`ConstexprEngine`
2. 支持标量局部（包括浮点值）、字符串字面量、数组、只读 span、`if`、`while`、`return`、递归
3. 在常量表达式上下文里同时支持检查型浮点算术 / 比较与整数算术
4. 对 unsafe / FFI / 动态分配路径显式拒绝
5. 把 `consteval` 调用接进语义检查
6. 加入预算超限诊断

可独立合并 / 测试：

- 整数 / 字符 / 浮点 / 字符串上的 immediate function
- 编译期字符串解析器微型测试
- imported module consteval helper 测试

## Phase E — 真正的 `constexpr` 与 required constant-expression context

1. 允许 `constexpr` 函数与构造函数
2. 为以下位置加入“这里要求常量表达式”的检查：
   - `constexpr` 变量初始化
   - 当 scpp 统一采用 constexpr-sized array 后的数组边界
   - 非类型模板实参（逐步取代当前那个专用小解释器）
3. 实现 `if consteval`
4. 实现 `std::is_constant_evaluated()` intrinsic 语义

可独立合并 / 测试：

- 同一个函数既可编译期调用也可运行期调用
- `if consteval` 的分支选择测试
- 浮点 constexpr 变量 / helper 测试

## Phase F — 与 `<format>` / `<print>` 标准库整合

1. 加入 `std::format_string<Args...>`
2. 把当前 runtime validator 改写成 constexpr-friendly helper
3. 让 `std::print` / `std::println` 改为接收 typed format-string parameter
4. 第一版先保持今天已支持的格式化子集不变，但把验证从运行期挪到编译期
5. 更新面向用户的 docs / book

可独立合并 / 测试：

- `std::print` / `std::println` 的正反向 blackbox tests
- imported `std` module tests

## Phase G — 扩展 constexpr 子集

后续如果需要，再逐步加入：

- constexpr lambda
- 更多 class 的构造 / 析构支持
- 更多标准库
- 更丰富的指针语义
- 如果性能分析显示 AST-walk 成瓶颈，再考虑 bytecode/MIR 层

## 7. 最终定稿设计决议

用户评审已经把最后几个分叉全部定下来了。最终设计决议如下：

1. **v1 的 class 支持边界保持原推荐方案：** constexpr-compatible 字段，加上 `constexpr` / `consteval` 构造函数，但 v1 不执行用户自定义析构逻辑。
2. **`.scppm` 的编译期载荷从一开始就采用结构化 AST 序列化，而不是继续复用源码快照。**
3. **generic body 与 constexpr body 的跨模块传输统一走这一套结构化序列化机制，不长期维护两套永久机制。**
4. **浮点 constexpr 算术进入 v1，而不是放到后续 widening phase。**

## 8. 最终建议

最务实的落地顺序是：

1. **先落 `.scppm` 的结构化 AST 序列化 / 反序列化能力，用来承载编译期相关载荷**；
2. **再落参数包推导重构**；
3. **第三步落前端 AST 解释器，直接把浮点支持也纳入 v1**；
4. **用 `std::format_string<Args...>` 作为第一证明用例**；
5. **先交付一个文档明确的子集，而不是等到“完整 C++ constexpr 对等”再发。**

这样得到的是一个“最小但已经通用”的架构：它足够接近真实 C++，不会让用户意外；又能直接解锁推动这次设计的 typed `std::print` / `std::println` 能力。

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
