# 6. v0.1 支持的 safe 子集

safe 区内**仅**支持下列语法；其余在 safe 区报 `E-UNSUPPORTED-IN-SAFE`
（明确区别于"不安全"，表示"尚未实现健全检查"）：

**类型**
- **标量基础类型**（数值家族的设计已定稿；`bool` 和 `char` 已实现，
  其余**尚未实现**）：

  | scpp 名字 | 含义 | 备注 |
  |-----------|------|------|
  | `bool` | 布尔，1 字节宽 | 已实现。`false` 是位模式 `0`，`true` 是 `1`。不支持到任何其它类型的隐式转换——跟真实 C++ 不一样（那边 `bool` 会隐式提升成 `int`，`if`/`while` 里任何标量也会隐式转成 `bool`），scpp 两个方向都要求显式转换，`if`/`while` 的条件必须本来就是 `bool`。 |
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
  | `char` | 字节值，1 字节宽 | **不是**`uint8_t`（或任何其它类型）的别名——是一个独立的类型，跟上面的 `bool` 一样不支持隐式转换：`char` 转成/转自任何其它类型都得显式转换。因为 `char` 不再需要跟 `uint8_t`/`int8_t` 共享同一个类型身份，真实 C++ 里裸 `char` 那种 implementation-defined 的符号性问题（典型 x86 工具链默认有符号，典型 ARM 工具链默认无符号）根本不会冒出来——反正没有隐式算术或比较会受它影响。这也顺带消解了之前标记的、跟正在并发进行的实现（把 `char` 写成有符号 `i8`）之间的冲突：既然 `char` 不再要求跟 `uint8_t` 是同一个类型，那个内部表示选择就不再跟规范冲突了。 |
  | *（不提供 `wchar_t`）* | -- | 故意完全不提供：真实 C++ 的 `wchar_t` 在 Windows 上是 2 字节/UTF-16，在 Linux/macOS 上是 4 字节/UTF-32——比上面 `long`/`char` 的坑还严重（不只是位宽不一样，连编码语义都不一样）。scpp 干脆不提供这个类型，而不是去武断地钉死一个选择。 |

- `struct`（规则见 [§4.1](ch04-struct-vs-class.md)；仅含受支持类型的
  字段）。
- `std::unique_ptr<T>`（已实现）、`std::span<T>`/`std::span<const T>`
  （已实现，M6 第一阶段——但目前只能从定长数组构造，见
  [§3](ch03-syntactic-sugar.md)）。`std::vector<T>` 规划中但**尚未
  实现**（现在只有定长数组 `T[N]`）。
- `std::expected<T, E>`（见 [§5.6](ch05-static-checks.md)——**设计已
  定稿，尚未实现**）：scpp 唯一的可恢复错误载体；是编译器内置类型，跟
  `unique_ptr`/`span` 待遇一样，不是真实 libstdc++/libc++ 模板的实例化。

**表达式 / 语句**
- 局部变量声明与初始化。
- `&` / `const &` 借用；`std::span`/`std::span<const T>` 视图。
- `&expr` 取地址，根据 `expr` 的 place 是只能只读访问还是可变访问，得到
  `const T*` 或者 `T*`（见 [§5.7](ch05-static-checks.md)——**设计已定稿，
  尚未实现**）：在 `safe` 函数里始终合法（不需要 `unsafe { }` 就能造出
  来——只有解引用裸指针才需要，见下面），是 `safe` 函数给 `extern "C"`
  输出参数产出指针值的具体办法。`const T*`/`T*` 是真正不同的两个类型
  （只有单向的隐式 `T* -> const T*` 转换，没有 `const_cast` 等价物）；
  通过 `const T*` 写是普通类型错误，无条件成立，哪怕在 `unsafe { }` 里
  也一样。
- `std::move`。
- 函数调用，包括 [§2](ch02-boundary-rules.md) 里"被调方须 `safe`，否则
  `unsafe {}`"这条规则（跟下面的 `unsafe { }` 一起已经实现）。
- 算术/逻辑/比较运算。
- `if` / `while` / `return`。（`for`/range-for **尚未实现**——目前只能用
  `while` 手写迭代；词法层面保留了 `for` 关键字，但 parser/AST 还没有
  对应的语句形式。）
- 成员访问、下标（定长数组、`span`，span 带运行时边界检查——见
  [§8](ch08-open-questions.md)）。
- `[[scpp::lifetime(name)]]` attribute，标在引用型形参/声明符上，用于
  跨函数的多组生命周期机制（见 [§5.3](ch05-static-checks.md)——**设计已
  定稿，尚未实现**）。
- `unsafe { }` 语句块（见 [§1.3](ch01-safety-context.md)，已实现）：在
  `safe` 函数内部开一个词法作用域的逃生窗口，局部放行裸指针解引用和
  调用非 `safe` 函数（这是 v0.1 里 [§5.5](ch05-static-checks.md) 禁止项
  中唯二能真正碰到的两条），[§5](ch05-static-checks.md) 里的其余检查
  照常无条件继续跑。
- `extern "C"` 函数声明/定义（见 [§2.1](ch02-boundary-rules.md)，已
  实现），签名类型限定为 C-ABI 兼容类型。`void`（作为返回类型和指针的
  指向类型）跟它一起已经实现；数组形参（`T[N]`）会退化成 `T*`，跟普通
  C++ 一样。
- **没有异常**（`throw`/`try`/`catch`）——从 scpp 里彻底排除，不是
  backlog 项：可恢复错误是 `std::expected<T, E>` 值，用普通 `if`/`else`
  传播（见 [§5.6](ch05-static-checks.md)）；它的返回值**强制要求检查**
  ——悄悄丢弃是编译错误，不是 lint，就好像每个这样的函数都隐式带了
  `[[nodiscard]]`。不可恢复的失败（违反约定、边界检查、构造/析构函数
  里的前置条件违反）改用 `abort()`（见 [§5.6](ch05-static-checks.md)/
  [§8](ch08-open-questions.md)）。

**暂不支持（safe 区 backlog）**
- 模板 / 泛型、`concept`。
- 用户自定义 `class` 的完整检查（构造/析构、方法体内部借用；见
  [§4.2](ch04-struct-vs-class.md)）。
- 继承、虚函数。
- lambda 捕获引用的生命周期检查。
- `shared_ptr` 的完整别名模型。
- [§5.3](ch05-static-checks.md) 定稿的 `[[scpp::lifetime(name)]]` 多组
  机制的**实现**（目前只有设计，还没写进编译器；在它落地之前，跨函数
  情形一律走单引用参数/`this` 省略规则或新的默认分组规则）。
- [§5.7](ch05-static-checks.md) 定稿的 `&expr` 取地址的**实现**（目前
  只有设计）——这是目前唯一明确剩下的、真正卡住 `extern "C"` 现实场景
  （比如 POSIX socket API 的输出参数）的硬阻塞点。
- 上面数值标量家族的**实现**（设计已定稿；`bool`/`char` 已完成，其余都
  还没开始）：`int8_t`/.../`int64_t`、`uint8_t`/.../`uint64_t`、
  `int`/`long`/`unsigned int`/`unsigned long` 这几个定宽别名、
  `float32_t`/`float64_t`（以及 `float`/`double` 别名）、`size_t`、
  `ptrdiff_t`。
- `for`/range-for、`std::vector`、`std::string`/`std::string_view`
  （`char` 已经落地，缺口已解除但还没开始做）。`reinterpret_cast`、
  `union`、裸 `new`/`delete`、全局变量目前完全没有语法支持，`unsafe { }`
  对它们的放行也就无从谈起，等各自语法落地后再说。
- [§5.6](ch05-static-checks.md) 定稿的 `std::expected<T, E>` 的**实现**
  （目前只有设计），包括强制检查规则，以及
  [§4.2](ch04-struct-vs-class.md) 里可失败构造的指导原则。

---

[← 上一章：safe 区的静态检查](ch05-static-checks.md) · [目录](README.md) · [下一章：编译管线 →](ch07-compilation-pipeline.md)
