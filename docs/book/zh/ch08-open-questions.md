# 8. 未决问题（Open Questions，需后续拍板）

1. **下标越界**：safe 区 `vector[i]` / `span[i]` 是插入运行时边界检查
   （像 Rust），还是要求用带检查的 API？**已定并实现（M6）**：`span[i]`
   默认插入运行时边界检查，越界调用 `abort()`（`vector` 还没实现，但
   会沿用同一策略）。
2. **整数溢出**：safe 区是否检查有符号溢出？倾向：debug 下 panic，release
   下按 wrapping/UB？需定。
3. **panic 模型**：越界/断言失败如何终止？`std::terminate` 还是自定义
   panic + 栈展开？**已定并实现（M6）**：直接调用 libc 的 `abort()`（比
   `std::terminate()` 更底层、不依赖 C++ 运行时的 terminate-handler 机制，
   效果一致——进程立即终止，不做栈展开）。
4. **内部可变性**：是否引入等价 `Cell`/`RefCell` 的机制承载合法可变别名？
5. **`safe` 与 `const` 的关系**：`const` 成员函数在 safe 区如何映射借用？
6. **ABI / 与现有 C++ 库互操作**：safe 代码调用第三方头文件（全是 unsafe）
   的工程化方式（是否全部视为 `unsafe`）。**已定**：`extern "C"`
   （[§2.1](ch02-boundary-rules.md)，设计已定稿）是 scpp 跟外部世界
   **唯一**的互操作机制；scpp 代码跨文件互相共享由
   [ch11](ch11-modules-and-libraries.md) 回答（设计已定稿）。跟**既有
   的、原样不改的 C++ 库**互操作这件事本身（任意 class、模板、重载、
   异常、RTTI）明确**不追求**——考虑过"把检查通过的 scpp 转成真实
   C++ 文本、交给 Clang 编译"这条路（能让这件事变简单，但代价是要
   把已经跑通的、直接产 LLVM IR 的 codegen 整个推翻重来），最后否决
   了：对 safe 区代码而言，自己直接产 LLVM IR 的优化上限严格更高
   （借用检查器凭自己 NLL 精度证明出来的别名事实，能对应到 LLVM 的
   scoped `alias.scope`/`noalias` metadata，这个精细度在 C++ 源码
   语法层面没有对应写法——连 `__restrict` 都够不到，因为 `__restrict`
   最终只能映射到更粗粒度的、整个参数生命周期的 `noalias` 属性）。
7. **语言/编译器命名、文件扩展名**。

---

[← 上一章：编译管线](ch07-compilation-pipeline.md) · [目录](README.md) · [下一章：MVP 里程碑 →](ch09-milestones.md)
