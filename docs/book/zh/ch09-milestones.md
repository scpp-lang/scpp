# 9. MVP 里程碑（实现顺序，端到端优先）

- **M0**：定死本规范 + 选定实现语言与 LLVM 绑定。
- **M1**：最小端到端。子集：标量 + 局部变量 + `if`/`while` + 函数 →
  AST → LLVM IR → 可执行文件返回值正确。**暂不含任何检查**，先打通
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
  `T*` 的解引用本身已经跟下面 M6 的 `unsafe {}` 一起落地；`&expr`
  取地址（从任意 place 造出 `T*` 剩下的那条路）设计仍未实现，见
  [§5.7](ch05-static-checks.md)。）**
- **M6**：`vector`/`span`/`string_view` 支持 + 边界检查策略 + 诊断质量。
  **（第一阶段已完成：`std::span<T>`/`std::span<const T>`——从定长数组
  构造、`.size` 字段、运行时边界检查 + `abort()` panic，详见
  [§3](ch03-syntactic-sugar.md)/[§6](ch06-safe-subset.md)/
  [§8](ch08-open-questions.md)。作为编译器内置类型实现（跟 `unique_ptr`
  一样），有意当作未来"泛型 + 泛型生命周期"机制的具体原型。另外，围绕
  "调用 C 库"这个具体目标（而不是原计划的 vector/string）也已完成：
  `unsafe {}` 语句块（[§1.3](ch01-safety-context.md)）、`extern "C"`
  函数声明/定义包括数组形参退化（[§2.1](ch02-boundary-rules.md)），以及
  `bool`/`char` 标量类型（[§6](ch06-safe-subset.md)）——`bool` 顺带从
  `i1` 改成了真正的 1 字节表示。`std::vector`、`std::string`/
  `string_view`（`char` 已解除阻塞）、`for`/range-for、`&expr` 取地址
  （设计已在 [§5.7](ch05-static-checks.md) 定稿——目前唯一剩下的、卡住
  真实 `extern "C"`/POSIX 场景的硬阻塞点），以及 `std::expected<T, E>`
  （设计已在 [§5.6](ch05-static-checks.md) 定稿）均留待后续。）**
- **M7+**：泛型/模板、trait/concept、`[[scpp::lifetime(name)]]` 多组
  跨函数生命周期机制（设计已在 [§5.3](ch05-static-checks.md) 定稿；尚未
  实现）、模块与库（设计已在 [ch11](ch11-modules-and-libraries.md) 定稿；
  尚未实现）、标准库扩展、增量编译。

---

[← 上一章：未决问题](ch08-open-questions.md) · [目录](README.md) · [下一章：参考实现 →](ch10-reference-implementations.md)
