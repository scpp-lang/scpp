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
- 函数调用（被调方须 `safe`，否则 `unsafe {}`）。
- 算术/逻辑/比较运算。
- `if` / `while` / `return`。（`for`/range-for **尚未实现**——目前只能用
  `while` 手写迭代；词法层面保留了 `for` 关键字，但 parser/AST 还没有
  对应的语句形式。）
- 成员访问、下标（定长数组、`span`，span 带运行时边界检查——见
  [§8](ch08-open-questions.md)）。

**暂不支持（safe 区 backlog）**
- 模板 / 泛型、`concept`。
- 用户自定义 `class` 的完整检查（构造/析构、方法体内部借用；见
  [§4.2](ch04-struct-vs-class.md)）。
- 继承、虚函数。
- 异常。
- lambda 捕获引用的生命周期检查。
- `shared_ptr` 的完整别名模型。
- 跨函数复杂生命周期（需显式标注的情形）。
- `for`/range-for、`char`/`float`/`double`、`std::vector`、
  `std::string`/`std::string_view`、`unsafe {}` 语句块（连带裸指针解
  引用）。

---

[← 上一章：safe 区的静态检查](ch05-static-checks.md) · [目录](README.md) · [下一章：编译管线 →](ch07-compilation-pipeline.md)
