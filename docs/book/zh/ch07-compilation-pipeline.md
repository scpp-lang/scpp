# 7. 编译管线（架构）

```
源码
 └─► 词法分析 (Lexer)
     └─► 递归下降 Parser ──► 统一 AST（safe/unsafe 同一套 AST，带 safety 标记）
         └─► 名称解析 + 类型检查 ──► HIR（去糖：std::move→move hint 等）
             ├─ [unsafe 区] ─────────────────────────────► 直接 lower
             └─ [safe 区] ─► MIR（CFG + 三地址）
                            └─► 借用检查（初始化/move/别名/生命周期）
                                └─► 检查通过后 lower
                 └─► LLVM IR ──► LLVM 优化 ──► 目标二进制
```

要点：
- **AST 统一**：safe 与 unsafe 代码共用一套 AST，节点上带 safety 上下文位。
- **借用检查只在 safe 区的 MIR 上进行**；unsafe 区跳过，直接下降。
- 前端对 unsafe/普通 C++ 只需"够用"——不追求完整 C++ 兼容。
- MIR 显式化：所有权转移、borrow 起止、drop 插入点、CFG。

---

[← 上一章：v0.1 支持的 safe 子集](ch06-safe-subset.md) · [目录](README.md) · [下一章：未决问题 →](ch08-open-questions.md)
