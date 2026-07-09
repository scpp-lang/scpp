# The SCPP Programming Language（scpp 语言教程）

本书把 scpp 当作一门今天就能上手编写的语言来讲解。前几章是循序渐进的教程主线，后面的章节则作为参考附录使用。需要精确、规范化表述时，请查看 `docs/spec/` 下的形式化规范。

> English version: [en/README.md](../en/README.md)

## 教程主线

1. [开始上手](ch00-design-philosophy.md)
2. [第一个完整的小程序](ch01-safety-context.md)
3. [基本构件](ch02-boundary-rules.md)

## 参考附录

4. [语法糖 / 既有语法的重新语义化](ch03-syntactic-sugar.md)
5. [struct 与 class 的语义区分（内存布局 / ABI 固定）](ch04-struct-vs-class.md)
6. [静态检查（健全性核心）](ch05-static-checks.md)
7. [v0.1 支持的子集](ch06-safe-subset.md)
8. [编译管线（架构）](ch07-compilation-pipeline.md)
9. [未决问题（Open Questions，需后续拍板）](ch08-open-questions.md)
10. [MVP 里程碑（实现顺序，端到端优先）](ch09-milestones.md)
11. [参考实现（务必先研读）](ch10-reference-implementations.md)
12. [模块与库（Modules & Libraries）](ch11-modules-and-libraries.md)
13. [IDE 集成](ch12-ide-integration.md)
14. [编译器调用与 CLI](ch13-compiler-invocation.md)
