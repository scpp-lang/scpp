# 6. v0.1 支持的子集

**仅**支持下列语法；其余报 `E-UNSUPPORTED`
（超出这个子集的构造——跟普通的类型/借用检查错误是两回事）：

**类型**
- **标量基础类型**（数值家族）：

  | scpp 名字 | 含义 | 备注 |
  |-----------|------|------|
  | `bool` | 布尔，1 字节宽 | `false` 是位模式 `0`，`true` 是 `1`。不支持到任何其它类型的隐式转换——跟真实 C++ 不一样（那边 `bool` 会隐式提升成 `int`，`if`/`while` 里任何标量也会隐式转成 `bool`），scpp 两个方向都要求显式转换，`if`/`while` 的条件必须本来就是 `bool`。 |
  | `int8_t` / `int16_t` / `int32_t` / `int64_t` | 定宽有符号整数 | 原样复用真实 C++ `<cstdint>` 的名字，都已经是标准化的。跟真实 C++ 不一样（那边定宽类型是条件性提供的），scpp 在任何 target 上都**无条件**保证这些类型存在——LLVM 原生支持任意位宽整数，没有哪个平台会逼 scpp 省掉其中任何一个。**暂不提供 `int128_t`**：WG21 P1467（128 位整数类型）还没被 C++26 采纳（scpp 参照哪个标准版本见 [ch00](ch00-design-philosophy.md) §7），scpp 的内置词汇表故意只用标准已经真正定下来的名字（见 [ch00](ch00-design-philosophy.md) §2/§6），不去提前对齐一个还悬而未决的提案将来到底叫什么——等以后哪个标准真的采纳了再加回来。 |
  | `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` | 定宽无符号整数 | 同上——同样的理由，暂不提供 `uint128_t` |
  | `int` | `int32_t` 的别名 | **不管目标平台是什么，含义都固定** |
  | `long` | `int64_t` 的别名 | **故意钉死**——真实 C++ 里 `long` 的位宽是平台决定的（Linux/macOS 的 LP64 是 64 位，Windows 的 LLP64 哪怕在 64 位机器上也是 32 位）。这正是 scpp 存在的意义要设计掉的那种跨平台天坑：scpp 保留了这个熟悉的拼写（看起来像 C++），但给它一个不管到哪个平台都一样、可预期的含义（不会因为换了目标平台就悄悄变了大小）。 |
  | `unsigned int` | `uint32_t` 的别名 | 跟 `int` 一样，不管平台，含义固定。跟真实 C++ 不一样，单独一个词的简写 `unsigned`（意思是 `unsigned int`）在 scpp 里**不合法**——只接受完整的两个词拼写，保证任何 `unsigned` 开头的东西都无歧义、方便 grep。 |
  | `unsigned long` | `uint64_t` 的别名 | 跟 `long` 一样，不管平台，含义固定 |
  | `float32_t` / `float64_t` | IEEE-754 binary32 / binary64 | 原样复用真实的、已经标准化的 C++23 `<stdfloat>` 名字 |
  | `float` | `float32_t` 的别名 | 跟真实 C++ 现实里的行为一致——不存在 `long` 那种平台分裂风险 |
  | `double` | `float64_t` 的别名 | 同上 |
  | `size_t` | 无符号、宽度等于指针宽度 | 跟真实 C++/Rust（`usize`）语义一致：这个类型就是**该**随目标 triple 的指针宽度变化的——这是它本来的职责，不是要消灭的坑。跟上面的 `long` 正好对比：问题形状一样（这个类型的位宽是不是随平台变），答案相反，因为这两个类型存在的理由不一样。 |
  | `ptrdiff_t` | 有符号、宽度等于指针宽度 | 跟 `size_t` 一样随目标 triple 变化 |
  | `char` | 字节值，1 字节宽 | **不是**`uint8_t`（或任何其它类型）的别名——是一个独立的类型，跟上面的 `bool` 一样不支持隐式转换：`char` 转成/转自任何其它类型都得显式转换。因为 `char` 不再需要跟 `uint8_t`/`int8_t` 共享同一个类型身份，真实 C++ 里裸 `char` 那种 implementation-defined 的符号性问题（典型 x86 工具链默认有符号，典型 ARM 工具链默认无符号）根本不会冒出来——反正没有隐式算术或比较会受它影响。 |
  | *（不提供 `wchar_t`）* | -- | 故意完全不提供：真实 C++ 的 `wchar_t` 在 Windows 上是 2 字节/UTF-16，在 Linux/macOS 上是 4 字节/UTF-32——比上面 `long`/`char` 的坑还严重（不只是位宽不一样，连编码语义都不一样）。scpp 干脆不提供这个类型，而不是去武断地钉死一个选择。 |

  **上表任何两个不同的标量类型之间，都没有隐式转换，没有例外。**
  这条规则本来是单独给 `bool`/`char` 说的；现在推广到整个数值家族——
  比如 `int8_t -> int16_t`、`int32_t -> float64_t`、
  `unsigned int -> long`——哪怕是变宽、不丢信息的方向，也一律要求显式
  cast。scpp 这里刻意照抄 Rust/Swift/Kotlin 的做法，不是真实 C++ 的
  隐式提升/"通常算术转换"那一套：真实 C++ 的提升规则只会精确指向
  `int`/`unsigned int`/`double`，不是"离得最近的更宽类型"，导致哪个
  重载会赢，取决于哪个内置类型恰好是这台机器的 `int`——而两个都只是
  "普通转换"级别的候选（比如 `int16_t` 和 `int64_t` 竞争一个
  `int32_t` 实参）在真实 C++ 里直接判歧义，压根没有"更近的赢"这条
  规则（见 [§8](ch08-open-questions.md) Q11）。完全不做隐式转换，让
  每一次转换都在调用处清清楚楚可见，顺带的效果是函数重载决议
  （[§5.10](ch05-static-checks.md)）被简化成了纯粹的类型精确匹配。

- `struct`（规则见 [§4.1](ch04-struct-vs-class.md)；仅含受支持类型的
  字段）。
- `class`（见
  [§4.2](ch04-struct-vs-class.md)/[§5.9](ch05-static-checks.md)）：成员
  变量和成员函数，各自可以是 `public`或者 `private`，任意搭配，跟真实
  C++ 完全一样；v0.1 没有继承/`protected`。
  平凡类型的成员可以改声明成 `mutable`（见
  [§4.2](ch04-struct-vs-class.md)/[§5.9](ch05-static-checks.md)）：
  通过 `const` 的 `this` 也能读写，但永远不能被引用，是 scpp 对内部
  可变性的第一阶段（`Cell` 等价）答案（[§8](ch08-open-questions.md)
  Q4）。
- 用于 FFI / 存储重叠工作的 `union`（见
  [§5.19](ch05-static-checks.md#519-union-与-scpppacked)）：普通安全代码可以
  声明、携带一个 union 值，但访问成员必须放在 `[[scpp::unsafe]]` 里，因为
  SCPP26 目前把所有 union 都按未加标签处理。`[[scpp::packed]]` 可以挂在
  `struct` 或 `union` 上，请求取消成员间 padding，并把整个聚合类型的总对齐
  降为 1。
- `std::unique_ptr<T>` 和 `std::make_unique<T>(...)`：由 `std` module
  通过 `import std;` 作为普通库代码提供，不是编译器内建类型。它的
  move-only 行为，只是 [§4.2](ch04-struct-vs-class.md) 那套通用 `class`
  规则的普通结果。
- `std::span<T>`/`std::span<const T>`（只能从定长数组构造，见
  [§3](ch03-syntactic-sugar.md)）。`std::vector<T>` 已推迟（v0.1 范围内
  只有定长数组 `T[N]`）。
- **泛型 `struct`/`class` 类型**（`template<typename T> class X { ... }`，
  原样复用真实 C++ 语法，支持多个类型参数和参数包——见
  [§5.14](ch05-static-checks.md)）：scpp 的编译期多态机制
  （[§5.11](ch05-static-checks.md)）从函数扩展到类型定义。类型参数可以
  裸写（只保证 move/存/传给兼容参数/return），也可以按方法各自用
  `requires`子句约束；泛型 `struct`自己的类型参数则必须绑 concept
  保证 trivial，因为 `struct`的字段-trivial 规则
  （[§4.1](ch04-struct-vs-class.md)）是整个类型的性质。变参泛型类型的
  存储靠递归继承实现（真实 C++ 没有能把 pack 直接展开成成员列表的
  语法）；非类型模板参数只支持标量类型。
- **`[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`/
  `[[scpp::thread_movable_if(a, b)]]`**，以及内置谓词
  `scpp::is_thread_movable(T)`/`scpp::is_thread_shareable(T)`（见
  [§5.15](ch05-static-checks.md)）：这些 attribute 可以用来约束一个
  泛型函数参数，或者覆盖一个 `struct`/`class` 自己推导出来的结果；那
  两个谓词则是对类型名求值的编译器 intrinsic（写法类似
  `__is_trivially_copyable(T)`），不是普通用户代码。让库代码（比如一个
  负责创建线程的函数）能通过普通的参数 attribute，去要求"传给我的东西，
  能安全地移动/共享给另一个线程"——对应 Rust 的 `Send`/`Sync`，以及它的
  `unsafe impl` 逃生舱。

**表达式 / 语句**
- 局部变量声明与初始化。
- `&` / `const &` 借用；`std::span`/`std::span<const T>` 视图。
- `&expr` 取地址，根据 `expr` 的 place 是只能只读访问还是可变访问，得到
  `const T*` 或者 `T*`（见 [§5.7](ch05-static-checks.md)）：始终合法（不需要 `[[scpp::unsafe]] { }` 就能造出
  来——只有解引用裸指针才需要，见下面），是普通（默认受检查）代码给
  `extern "C"` 输出参数产出指针值的具体办法。`const T*`/`T*` 是真正
  不同的两个类型（只有单向的隐式 `T* -> const T*` 转换，没有
  `const_cast` 等价物）；通过 `const T*` 写是普通类型错误，无条件
  成立，哪怕在 `[[scpp::unsafe]] { }` 里也一样。
- `std::move`。
- 函数调用，包括 [§2](ch02-boundary-rules.md) 里"调用 `extern "C"`
  函数需要 `[[scpp::unsafe]] {}`"这条规则。
  函数（自由函数或方法）可以按参数列表**重载**，永远不能只靠返回类型
  区分（见 [§5.10](ch05-static-checks.md)）：
  只按类型精确匹配解析，因为 scpp 标量类型之间没有隐式转换（见上面
  数值家族那条备注）——纯类型不匹配导致的歧义因此不可能出现。
- **受 `concept` 约束的泛型函数**（`void f(Shape auto& x)`，原样复用
  真实 C++20 语法——见 [§5.11](ch05-static-checks.md)）：scpp 用来代替继承/虚函数（仍然不在 v0.1 范围内，见下面）的
  编译期多态机制。按每个具体类型单态化（零开销，没有 vtable）；受约束
  的函数体在它自己定义的地方就检查一遍，只认 concept 的 `requires`
  表达式保证过的东西——不像真实 C++ 模板那样延迟到实例化才检查。受约束
  的参数也可以是参数包（`Concept auto&... args`），可通过 fold
  expression 使用（原样复用真实 C++17 语法）。完整 header 形式现在也
  支持参数包（`template<typename... Args> ... Args... args`），包括把
  这个 pack 继续转发进另一个实参列表（如 `g(args...)` 或
  `new T(args...)`）；函数体内部递归拆包依然不支持。
- **lambda 表达式**（`[capture-list](params) { body }`，原样复用真实
  C++ 语法——见 [§5.12](ch05-static-checks.md)）：跟真实 C++ 一样脱糖
  成一个匿名的、编译器合成的类，所以除了 `struct`/`class` 已有的规则
  （[§4](ch04-struct-vs-class.md)）之外不需要任何新检查机制。按值
  capture是普通的拥有型成员；按引用capture是引用类型的成员，使闭包值
  本身跟 `std::span` 一样是生命周期追踪的。`this`/`*this` 必须显式
  capture——裸的 `[=]`/`[&]` 隐式capture `this` 是编译错误（真实
  C++20 只是把它标记为 deprecated，P0806R2）。
- `consteval` 函数（见 [§4.2](ch04-struct-vs-class.md)）：scpp 唯一的编译期函数机制，原样复用真实 C++20 语法——
  每次调用都强制在编译期求值，只要有一个实参本身不是常量表达式就编译
  错误。scpp**没有 `constexpr` 修饰的函数**：真实 C++ 的 `constexpr`
  函数在需要常量表达式的上下文里能编译期求值，别的地方就静默退化成
  普通运行期调用——**具体走哪条路取决于调用点的上下文**，从函数自己
  的声明上完全看不出来，这正是 scpp 别处也一直想避免的那种"看上下文
  才知道行为"的模糊性（见 [ch00](ch00-design-philosophy.md) §8）。
  scpp 里每个函数都得明确二选一：`consteval`，或者一个不加任何修饰的
  普通函数，永远是运行期调用，哪怕所有实参碰巧都是常量也不会在编译期
  求值。`constexpr` **变量**不受影响——`constexpr int x = 5;` 在真实
  C++ 里本来就没有歧义（永远是编译期常量，不依赖调用上下文），不需要
  修。如果以后真的出现"同一个函数编译期运行期都要用"的硬需求，到时候
  再考虑把 `constexpr` 函数加回来，不用现在想别的办法解决。
- 算术/逻辑/比较运算。`+`/`-`/`*` 默认检查溢出（溢出就
  `abort()`，有符号无符号都查；见 [§5.8](ch05-static-checks.md)）；在 `[[scpp::unsafe]] { }` 里不检查，但保证 wrap
  （绝不是 UB）。除以 0/模以 0（或者 `INT_MIN / -1`）无条件 `abort()`，
  不管在不在 `[[scpp::unsafe]] { }` 里都一样。
- `if` / `while` / `return`。（`for`/range-for 不在 v0.1 范围内——目前只能用
  `while` 手写迭代。）
- 成员访问、下标（定长数组、`span`，span 默认带运行时边界
  检查，在 `[[scpp::unsafe]] { }` 里跳过——见
  [§8](ch08-open-questions.md)）。
- 对声明了 `operator*()`/`operator*() const` 的 `class` 使用一元 `*`
  和 `->`（见 [§5.17](ch05-static-checks.md)）：`*x` 脱糖成一次普通方法
  调用，`x->y` 脱糖成 `(*x).y`。没有单独的 `operator->` 特性，其它
  运算符名字的重载（比如 `operator+`）也不在这一轮范围内。
- 带 `[[scpp::unsafe]]` attribute 的语句块（见 [§1.3](ch01-safety-context.md)）：
  一个词法作用域的逃生窗口，局部放行裸指针解引用、union 成员访问，以及
  调用一个 `extern "C"` 函数（这是 [§5.5](ch05-static-checks.md)
  禁止项列表里在 v0.1 范围内的部分，列表其余部分不在 v0.1 范围内，见下面），
  [§5](ch05-static-checks.md) 里的其余
  检查照常无条件继续跑。
- 函数级 `[[scpp::unsafe]]` 标记（见 [§1.2](ch01-safety-context.md)）：
  同一个 attribute，改挂在函数自己的声明上，让整个函数体变成 unsafe
  context，也让调用这个函数本身变成 [§5.5](ch05-static-checks.md) 里的
  一项受管制操作——对应 Rust 的 `unsafe fn`，用在函数的健全性依赖某个
  只有调用者才能保证的前置条件的场景。
- `extern "C"` 函数声明/定义（见 [§2.1](ch02-boundary-rules.md)），
  签名类型限定为 C-ABI 兼容类型，包括 `void`（作为返回类型和指针的
  指向类型）；数组形参（`T[N]`）会退化成 `T*`，跟普通
  C++ 一样。
- **没有异常**（`throw`/`try`/`catch`）——从 scpp 里彻底排除，不是
  backlog 项。语言设计里，可恢复错误的答案是带强制检查的
  `std::expected<T, E>`（见 [§5.6](ch05-static-checks.md)），但
  `std::expected` 还不属于当前编译器已经支持的子集。不可恢复的失败
  （违反约定、边界检查、构造/析构函数里的前置条件违反）改用 `abort()`
  （见 [§5.6](ch05-static-checks.md)/[§8](ch08-open-questions.md)）。

**不在 v0.1 范围内**
- 任意/通用的模板特化（超出变参泛型类型能用的那种固定空
  pack/Head+Tail 模式，见 [§5.14](ch05-static-checks.md)）、模板模板
  参数、默认模板实参、class 类型的非类型模板参数、关联类型，以及
  函数体内的递归拆包——这些
  [§5.11](ch05-static-checks.md)/[§5.14](ch05-static-checks.md) 里泛型
  设计也明确排除在外。
- `class` 类型的继承和虚函数（因此也包括 `protected`）——见
  [§4.2](ch04-struct-vs-class.md)。
- `shared_ptr` 的完整别名模型。
- `std::expected<T, E>` 可恢复错误（设计见
  [§5.6](ch05-static-checks.md)）——当前编译器/stdlib 还没有提供这个类型。
- [§5.3](ch05-static-checks.md) 的通用
  `[[scpp::lifetime(name)]]` 多组跨函数生命周期机制。今天编译器真正实现的，
  仍只是旧的"单个引用形参 / `this`-elision"子集；
  `[[scpp::lifetime(name)]]` 目前只为前向兼容而被解析。
- `for`/range-for、`std::vector`、`std::string`/`std::string_view`、
  `reinterpret_cast`、裸 `new`/`delete`、全局变量。

---

[← 上一章：静态检查](ch05-static-checks.md) · [目录](README.md) · [下一章：编译管线 →](ch07-compilation-pipeline.md)
