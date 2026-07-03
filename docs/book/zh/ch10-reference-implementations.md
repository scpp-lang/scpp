# 10. 参考实现（务必先研读）

- **Circle**（Sean Baxter）：C++ 超集 + borrow checker，与本设计几乎同路。
- **Hylo / Val**：mutable value semantics，规避借用检查复杂度的另一思路。
- **cppfront / cpp2**（Herb Sutter）：C++ 语法翻新 + 安全默认。
- **Rust `rustc` borrowck + Polonius 论文/NLL RFC**：借用检查的成熟实现。
- **MLIR**：若后端想用高层 dialect 承载所有权/生命周期再逐步 lower。

---

*状态：草案 v0.1 · 待评审后进入 M1。*

---

[← 上一章：MVP 里程碑](ch09-milestones.md) · [目录](README.md)
