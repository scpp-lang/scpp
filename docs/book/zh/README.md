# The SCPP Programming Language（scpp 语言教程）

本书把 scpp 当作一门今天就能上手编写的语言来讲解。需要精确、规范化表述时，
请查看 `docs/spec/` 下的形式化规范。

> English version: [en/README.md](../en/README.md)

## 建议先读这里

1. [开始上手](ch00-design-philosophy.md)
2. [第一个完整的小程序](ch01-safety-context.md)
3. [基本构件](ch02-boundary-rules.md)

## 现有参考章节

下面这些章节仍然保留了大量偏参考手册风格的内容；在书的前半部分逐步改写为教
程体例的同时，它们依然是有价值的补充材料。

- [语法糖 / 既有语法的重新语义化](ch03-syntactic-sugar.md)
- [struct 与 class 的语义区分（内存布局 / ABI 固定）](ch04-struct-vs-class.md)
- [静态检查（健全性核心）](ch05-static-checks.md)
- [v0.1 支持的子集](ch06-safe-subset.md)
- [编译管线（架构）](ch07-compilation-pipeline.md)
- [未决问题（Open Questions，需后续拍板）](ch08-open-questions.md)
- [MVP 里程碑（实现顺序，端到端优先）](ch09-milestones.md)
- [参考实现（务必先研读）](ch10-reference-implementations.md)
- [模块与库（Modules & Libraries）](ch11-modules-and-libraries.md)
- [IDE 集成](ch12-ide-integration.md)
- [编译器调用与 CLI](ch13-compiler-invocation.md)
