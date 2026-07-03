# 1. 安全上下文（Safety Context）

编译器为每个函数体、lambda、块、类型维护一个 **safety context**，取值：

- **`unsafe`（默认）**：普通 C++ 语义。不做任何所有权/借用/别名检查。
- **`safe`**：启用全部静态安全检查（见 [§5](ch05-static-checks.md)）。

## 1.1 上下文的确定规则

- 未标注的函数/块 → `unsafe`（默认）。
- `safe` 标注的函数/块 → `safe`。
- 子块继承父块的上下文，除非被显式标注覆盖。
- `unsafe { }` 块在 `safe` 上下文中开一个逃生窗口，恢复为 `unsafe` 语义。
- （暂不支持）`safe { }` 块在 `unsafe` 上下文中局部开启检查——留待后续版本。

## 1.2 标注位置

```cpp
safe int f(...);                 // 自由函数
struct S { safe void g(); };     // 成员函数
safe struct Widget { ... };      // 类型级：所有成员函数默认 safe（v0.2 再定）
auto lam = safe [](int x){...};  // lambda（v0.2 再定）
```

v0.1 **只要求实现函数级 `safe` 和块级 `unsafe { }`**。其余标注位置进入
backlog。

---

[← 上一章：设计理念](ch00-design-philosophy.md) · [目录](README.md) · [下一章：边界规则 →](ch02-boundary-rules.md)
