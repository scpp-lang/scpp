# The SCPP Programming Language（scpp 语言教程）

欢迎来到 *The SCPP Programming Language*。

scpp 想做一件很不寻常的事：尽量保留现代 C++ 的表面手感，同时把 Rust 风格的
所有权、借用和生命周期检查变成默认状态。这本书会把这个想法当成一门你今天就能
真的写、真的编译、真的运行的语言来讲。

## 这本书写给谁

这本书首先写给那些已经认得普通 C++ 语法、又想理解 scpp 在**语义上**做了什么改
变的读者。

你**不**需要是编译器工程师，也**不**需要先学过 Rust。只要你已经知道函数、变量、
编译器这些基本概念，前面的章节就会用可运行的小程序一步步带你进入状态。

## 这本书怎么读

这本书分成两层：

- 前面的**教程主线**先从真实程序讲起，只在变得有用的时候再引入语言规则；
- 后面的章节继续保留设计说明、路线图和偏参考手册的细节，适合你想看更完整全貌时
  再回来查阅。

需要精确、规范化表述时，请查看 `docs/spec/` 下的形式化规范。
English version: [en/README.md](../en/README.md)

- 教程主线
  - [开始上手](ch00-design-philosophy.md)
  - [第一个完整的小程序](ch01-safety-context.md)
  - [基本构件](ch02-boundary-rules.md)
  - [熟悉语法背后的借用、移动与视图](ch03-syntactic-sugar.md)
  - [struct 与 class 的语义区分（内存布局 / ABI 固定）](ch04-struct-vs-class.md)
  - [静态检查（健全性核心）](ch05-static-checks.md)
- 设计说明与参考资料
  - [v0.1 支持的子集](ch06-safe-subset.md)
  - [编译管线（架构）](ch07-compilation-pipeline.md)
  - [未决问题（Open Questions，需后续拍板）](ch08-open-questions.md)
  - [MVP 里程碑（实现顺序，端到端优先）](ch09-milestones.md)
  - [参考实现（务必先研读）](ch10-reference-implementations.md)
  - [模块与库（Modules & Libraries）](ch11-modules-and-libraries.md)
  - [IDE 集成](ch12-ide-integration.md)
  - [编译器调用与 CLI](ch13-compiler-invocation.md)
