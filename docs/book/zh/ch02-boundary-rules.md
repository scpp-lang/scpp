# 2. 边界规则（Safe ↔ Unsafe 交互）

这是健全性的关键，必须严格。

| 调用方向 | 规则 |
|----------|------|
| `unsafe` 调 `safe` | **自由放行**。safe 函数对任何调用者都安全。 |
| `safe` 调 `safe` | 自由放行，正常参与检查。 |
| `safe` 调 `unsafe` | **必须包在 `unsafe { }` 内**，否则编译错误。程序员以此背书。 |
| `safe` 内解裸指针 | 必须在 `unsafe { }` 内。 |

- 边界处的数据契约：safe 函数暴露给 unsafe 世界的引用/指针，其生命周期
  义务对 unsafe 侧**不强制**（unsafe 侧自负）。反之，unsafe 传入 safe 的
  引用，safe 侧**假定其在函数调用期间有效**（调用者义务）。
- 编译器需能标记一个 `unsafe` 函数是否"已人工审核为可安全调用"——v0.1
  不做形式化，先靠 `unsafe { }` 背书。

---

[← 上一章：安全上下文](ch01-safety-context.md) · [目录](README.md) · [下一章：语法糖 →](ch03-syntactic-sugar.md)
