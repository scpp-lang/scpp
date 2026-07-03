# 6. v0.1 支持的 safe 子集

safe 区内**仅**支持下列语法；其余在 safe 区报 `E-UNSUPPORTED-IN-SAFE`
（明确区别于"不安全"，表示"尚未实现健全检查"）：

**类型**
- 基础标量：`bool`、`int`。（`float`/`char` 规划中但**尚未实现**——没有
  对应的词法/类型支持；`std::string`/`std::string_view` 需要先有 `char`
  类型，同样未实现。）
- `struct`（规则见 [§4.1](ch04-struct-vs-class.md)；仅含受支持类型的
  字段）。
- `std::unique_ptr<T>`（已实现）、`std::span<T>`/`std::span<const T>`
  （已实现，M6 第一阶段——但目前只能从定长数组构造，见
  [§3](ch03-syntactic-sugar.md)）。`std::vector<T>` 规划中但**尚未
  实现**（现在只有定长数组 `T[N]`）。

**表达式 / 语句**
- 局部变量声明与初始化。
- `&` / `const &` 借用；`std::span`/`std::span<const T>` 视图。
- `std::move`。
- 函数调用。（[§2](ch02-boundary-rules.md) 里"被调方须 `safe`，否则
  `unsafe {}`"这条规则**尚未强制执行**——目前从 `safe` 函数调用非
  `safe` 函数完全不会被拒绝；这条规则会跟 `unsafe { }` 一起落地，见
  下面。）
- 算术/逻辑/比较运算。
- `if` / `while` / `return`。（`for`/range-for **尚未实现**——目前只能用
  `while` 手写迭代；词法层面保留了 `for` 关键字，但 parser/AST 还没有
  对应的语句形式。）
- 成员访问、下标（定长数组、`span`，span 带运行时边界检查——见
  [§8](ch08-open-questions.md)）。
- `[[scpp::lifetime(name)]]` attribute，标在引用型形参/声明符上，用于
  跨函数的多组生命周期机制（见 [§5.3](ch05-static-checks.md)——**设计已
  定稿，尚未实现**）。
- `unsafe { }` 语句块（见 [§1.3](ch01-safety-context.md)——**设计已定稿，
  尚未实现**）：在 `safe` 函数内部开一个词法作用域的逃生窗口，局部放行
  裸指针解引用和调用非 `safe` 函数（这是 v0.1 里
  [§5.5](ch05-static-checks.md) 禁止项中唯二能真正碰到的两条），
  [§5](ch05-static-checks.md) 里的其余检查照常无条件继续跑。
- `extern "C"` 函数声明/定义（见 [§2.1](ch02-boundary-rules.md)——**设计
  已定稿，尚未实现**），签名类型限定为 C-ABI 兼容类型。需要先有 `extern`
  关键字、最小限度的字符串字面量词法支持、以及 `void` 类型（这三样现在
  都还没有，见下面）。

**暂不支持（safe 区 backlog）**
- 模板 / 泛型、`concept`。
- 用户自定义 `class` 的完整检查（构造/析构、方法体内部借用；见
  [§4.2](ch04-struct-vs-class.md)）。
- 继承、虚函数。
- 异常。
- lambda 捕获引用的生命周期检查。
- `shared_ptr` 的完整别名模型。
- [§5.3](ch05-static-checks.md) 定稿的 `[[scpp::lifetime(name)]]` 多组
  机制的**实现**（目前只有设计，还没写进编译器；在它落地之前，跨函数
  情形一律走单引用参数/`this` 省略规则或新的默认分组规则）。
- [§1.3](ch01-safety-context.md) 定稿的 `unsafe { }` 语句块的**实现**
  （目前只有设计）。
- [§2.1](ch02-boundary-rules.md) 定稿的 `extern "C"` 的**实现**（目前
  只有设计），以及它的三个前置条件：`extern` 关键字、最小限度的字符串
  字面量词法支持（只认 `"C"` 这个 token，不是通用字符串字面量）、以及
  `void` 作为合法类型名（现在既没法声明返回 `void` 的函数，也没有
  `void*`，跟 `extern "C"` 无关，是独立的缺口）。
- `for`/range-for、`char`/`float`/`double`、`std::vector`、
  `std::string`/`std::string_view`。`reinterpret_cast`、`union`、裸
  `new`/`delete`、全局变量目前完全没有语法支持，`unsafe { }` 对它们的
  放行也就无从谈起，等各自语法落地后再说。

---

[← 上一章：safe 区的静态检查](ch05-static-checks.md) · [目录](README.md) · [下一章：编译管线 →](ch07-compilation-pipeline.md)
