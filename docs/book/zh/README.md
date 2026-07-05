# The SCPP Programming Language（scpp 语言规范）

*语言规范草案 v0.1（工作代号待定）*

> 一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是
> `unsafe` 关键字）。每个函数默认都启用 Rust 式健全（sound）的编译期
> 安全检查；`unsafe { }` 块局部放宽一份固定、狭窄的操作清单。后端经
> LLVM 生成本地二进制。
>
> English version: [en/README.md](../en/README.md)

*状态：草案 v0.1 · 待评审后进入 M1。*

## 目录

0. [设计理念（不可动摇的北极星）](ch00-design-philosophy.md)
1. [安全上下文（Safety Context）](ch01-safety-context.md)
2. [边界规则（跟 `unsafe { }` 与 `extern "C"` 的交互）](ch02-boundary-rules.md)
3. [语法糖 / 既有语法的重新语义化](ch03-syntactic-sugar.md)
4. [struct 与 class 的语义区分（内存布局 / ABI 固定）](ch04-struct-vs-class.md)
5. [静态检查（健全性核心）](ch05-static-checks.md)
6. [v0.1 支持的子集](ch06-safe-subset.md)
7. [编译管线（架构）](ch07-compilation-pipeline.md)
8. [未决问题（Open Questions，需后续拍板）](ch08-open-questions.md)
9. [MVP 里程碑（实现顺序，端到端优先）](ch09-milestones.md)
10. [参考实现（务必先研读）](ch10-reference-implementations.md)
11. [模块与库（Modules & Libraries）](ch11-modules-and-libraries.md)
