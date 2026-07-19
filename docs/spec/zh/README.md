# SCPP26 规范

*正式规范——工作草案。*

> `docs/spec/` 下面并排维护三份 SCPP26 规范文档。第一份是语言标准本身：
> 用 ISO 风格条款描述 SCPP26 如何修改 C++26。另两份则是独立的文件格式规范，
> 分别定义 `.scppm` 与 `.scppkg`。
>
> English version: [en/README.md](../en/README.md)

## 语言标准

本文档把 SCPP26 定义为 ISO/IEC 14882:2026（C++26），按下面的条款修改而成。
它是[那本书](../../book/zh/README.md)的配套文档——书负责教你怎么用这门语言；
这份文档负责精确地、规范性地说清楚它相对 C++ 标准到底改了什么。条款是逐步
增补的；某个主题这里还没有对应条款，不代表它没设计（书里可能已经讲过），
只是还没在这份文档里正式化。

### 目录

0. [前言部分（适用范围、规范性引用文件、术语和定义、一致性）](00-front-matter.md)
5. [`[[scpp::unsafe]]` Attribute](01-unsafe.md)
6. [所有权、初始化与 Move](02-ownership-and-move.md)
7. [解引用与成员访问](03-dereference-and-member-access.md)
8. [线程安全属性](04-thread-safety-properties.md)
9. [union 类型与 packed 布局](05-unions-and-packed-layout.md)
7. [常量求值](06-constant-evaluation.md)
9. [`constexpr` 与 `consteval` 说明符](07-constexpr-and-consteval.md)
13. [函数模板实参推导](08-function-template-argument-deduction.md)
14. [枚举转换](09-enumeration-conversions.md)
8. [迭代语句](10-iteration-statements.md)
11. [继承与接口](11-inheritance-and-interfaces.md)
10. [模块与命名空间](12-modules-and-namespaces.md)

## 文件格式规范

- [`.scppm` 模块接口格式](scppm-format.md)
- [`.scppkg` 包格式](scppkg-format.md)
