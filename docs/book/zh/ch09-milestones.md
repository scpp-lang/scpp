# 9. MVP 里程碑（实现顺序，端到端优先）

- **M0**：定死本规范 + 选定实现语言与 LLVM 绑定。
- **M1**：最小端到端。子集：标量 + 局部变量 + `if`/`while` + 函数 →
  AST → LLVM IR → 可执行文件返回值正确。**暂不含任何 safe 检查**，先打通
  前后端。
- **M2**：类型系统 + `struct` + `unique_ptr` + move 语义（`std::move` 作为
  hint），实现 **move-out 检查**（最简单的健全检查）。
- **M3**：建 MIR + 初始化检查 + drop 插入。
- **M4**：借用与别名 XOR 可变检查（函数内）。**（已完成）**
- **M5**：NLL 风格生命周期推断 + 悬垂引用检查（函数内）+ 省略规则。
  **（已完成：活跃性驱动的借用释放、函数返回引用的省略规则与悬垂检查、
  `a.b`/`arr[i]` 借用、引用指向 `std::unique_ptr` 指向的对象（`*p`/
  `p->x`）、调用返回引用的函数的结果可绑定新引用/继续传引用实参，详见
  [§5.2](ch05-static-checks.md)/[§5.3](ch05-static-checks.md)。裸指针
  `T*` 的解引用留待 `unsafe {}` 语句块实现之后。）**
- **M6**：`vector`/`span`/`string_view` 支持 + 边界检查策略 + 诊断质量。
  **（第一阶段已完成：`std::span<T>`/`std::span<const T>`——从定长数组
  构造、`.size` 字段、运行时边界检查 + `abort()` panic，详见
  [§3](ch03-syntactic-sugar.md)/[§6](ch06-safe-subset.md)/
  [§8](ch08-open-questions.md)。作为编译器内置类型实现（跟 `unique_ptr`
  一样），有意当作未来"泛型 + 泛型生命周期"机制的具体原型。`std::vector`、
  `std::string`/`string_view`（需要先有 `char` 类型）、`for`/range-for、
  `unsafe {}` 语句块均留待后续。）**
- **M7+**：泛型/模板、trait/concept、跨函数生命周期、标准库扩展、增量编译。

---

[← 上一章：未决问题](ch08-open-questions.md) · [目录](README.md) · [下一章：参考实现 →](ch10-reference-implementations.md)
