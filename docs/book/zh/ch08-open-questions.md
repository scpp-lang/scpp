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
8. **可恢复错误怎么处理**：走异常，还是走值类型？**已定**：scpp 哪里都
   没有异常（没有 `throw`/`try`/`catch`）。所有失败要么是**bug**（abort——
   已经由上面的 Q3 定了），要么是**可恢复的、预期内的情况**（一个普通的
   `std::expected<T, E>` 值，由编译器强制检查——见
   [§5.6](ch05-static-checks.md)）。构造函数/析构函数遵循同样的划分（见
   [§4.2](ch04-struct-vs-class.md)）：可以在前置条件不满足时 abort，但
   不能产生可恢复错误——可失败的构造改走一个返回 `std::expected<T, E>`
   的 `static` 工厂函数（经典 C++ "named constructor idiom"）。**传播**
   一个 `std::expected` 的错误到调用者，v0.1 用普通 `if`/`else`——曾经
   考虑过一个类似 Rust `?` 的后缀运算符（拼成 `??`，因为 C++ 已经用裸
   `?` 表示三元运算符了），最后**否决**了：跟 scpp 其它所有语法不一样，
   一个全新的运算符 token 没法被真正的 C++ 编译器忽略或者擦除掉，这会
   永久打破"把 `safe`/`unsafe` 从 scpp 文件里去掉，剩下的就是能被真 C++
   编译器原样接受的普通文件"这条性质（见 [ch00](ch00-design-philosophy.md)
   §2）——真编译器解析到第二个 `?` 就会硬报错（trigraph 是唯一曾经赋予
   `??` 含义的东西，C++17 就删掉了）。要不要重新考虑，等 C++ 标准自己在
   这块进一步演进之后再看。
9. **`const T*` 和 `T*` 是同一个类型吗？** [§5.7](ch05-static-checks.md)
   （`&expr` 设计）早先的草稿曾经假设 scpp 的 `const T*`/`T*` 统一成了
   一个不追踪的类型——这是错的，不管在真实 C++ 还是 scpp 里都不是，是在
   讨论中被发现并纠正的。真实 C++ 一直把它们当成两个不同的类型（单向
   隐式 `T* -> const T*` 转换，反过来要 `const_cast`），Rust 的
   `*const T`/`*mut T` 在编译期强制同样的划分——哪怕在 `unsafe` 里，通过
   `*const T` 写也会被拒绝。**已定**：scpp 认真追踪这个区分（新增一个
   `is_mutable_pointee` 标记，照抄 `is_mutable_ref` 区分 `T&`/`const T&`
   的方式）；单向隐式转换是真实 C++ 本来就有的规则，不是新发明的；通过
   `const T*` 写是普通的、无条件强制的类型错误，不是 `unsafe { }` 会
   放宽的东西。v0.1 没有 `const_cast` 等价物（见
   [§5.7](ch05-static-checks.md)）。

---

[← 上一章：编译管线](ch07-compilation-pipeline.md) · [目录](README.md) · [下一章：MVP 里程碑 →](ch09-milestones.md)
