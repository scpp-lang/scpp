# 8. 未决问题（Open Questions，需后续拍板）

1. **下标越界**：safe 区 `vector[i]` / `span[i]` 是插入运行时边界检查
   （像 Rust），还是要求用带检查的 API？倾向：safe 区默认插入边界检查。
2. **整数溢出**：safe 区是否检查有符号溢出？倾向：debug 下 panic，release
   下按 wrapping/UB？需定。
3. **panic 模型**：越界/断言失败如何终止？`std::terminate` 还是自定义
   panic + 栈展开？v0.1 先用 `std::terminate`（abort）。
4. **内部可变性**：是否引入等价 `Cell`/`RefCell` 的机制承载合法可变别名？
5. **`safe` 与 `const` 的关系**：`const` 成员函数在 safe 区如何映射借用？
6. **ABI / 与现有 C++ 库互操作**：safe 代码调用第三方头文件（全是 unsafe）
   的工程化方式（是否全部视为 `unsafe`）。
7. **语言/编译器命名、文件扩展名**。

---

[← 上一章：编译管线](ch07-compilation-pipeline.md) · [目录](README.md) · [下一章：MVP 里程碑 →](ch09-milestones.md)
