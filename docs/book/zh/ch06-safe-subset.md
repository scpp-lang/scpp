# 6. v0.1 支持的 safe 子集

safe 区内**仅**支持下列语法；其余在 safe 区报 `E-UNSUPPORTED-IN-SAFE`
（明确区别于"不安全"，表示"尚未实现健全检查"）：

**类型**
- 基础标量：`bool`、整型、浮点、`char`。
- `struct`（规则见 [§4.1](ch04-struct-vs-class.md)；仅含受支持类型的
  字段）。
- `std::unique_ptr<T>`、`std::vector<T>`、`std::span<T>`、`std::string_view`、
  `std::string`（最小子集）。

**表达式 / 语句**
- 局部变量声明与初始化。
- `&` / `const &` 借用。
- `std::move`。
- 函数调用（被调方须 `safe`，否则 `unsafe {}`）。
- 算术/逻辑/比较运算。
- `if` / `while` / `for`（含 range-for）/ `return`。
- 成员访问、下标（`vector`/`span`，带边界语义——运行时检查策略见
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

---

[← 上一章：safe 区的静态检查](ch05-static-checks.md) · [目录](README.md) · [下一章：编译管线 →](ch07-compilation-pipeline.md)
