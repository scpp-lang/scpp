# 9. MVP 里程碑（实现顺序，端到端优先）

- **M0**：定死本规范 + 选定实现语言与 LLVM 绑定。
- **M1**：最小端到端。子集：标量 + 局部变量 + `if`/`while` + 函数 →
  AST → LLVM IR → 可执行文件返回值正确。**暂不含任何检查**，先打通
  前后端。
- **M2**：类型系统 + `struct` + `unique_ptr` + move 语义（`std::move` 作为
  hint），move-out 检查（最简单的健全检查）。
- **M3**：建 MIR + 初始化检查 + drop 插入。
- **M4**：借用与别名 XOR 可变检查（函数内）。
- **M5**：NLL 风格生命周期推断 + 悬垂引用检查（函数内）+ 省略规则——见
  [§5.2](ch05-static-checks.md)/[§5.3](ch05-static-checks.md)。
- **M6**：`vector`/`span`/`string_view` 支持 + 边界检查策略 + 诊断质量——见
  [§3](ch03-syntactic-sugar.md)/[§6](ch06-safe-subset.md)/
  [§8](ch08-open-questions.md)。
- **M7+**：泛型/模板、trait/concept、`[[scpp::lifetime(name)]]` 多组
  跨函数生命周期机制（见 [§5.3](ch05-static-checks.md)）、模块与库
  （见 [ch11](ch11-modules-and-libraries.md)）、标准库扩展、增量编译。

---

[← 上一章：未决问题](ch08-open-questions.md) · [目录](README.md) · [下一章：参考实现 →](ch10-reference-implementations.md)
