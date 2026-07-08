# SCPP26 语言标准

*正式规范——工作草案。*

> 本文档把 SCPP26 定义为 ISO/IEC 14882:2026（C++26），按下面的条款修改
> 而成。它是[那本书](../../book/zh/README.md)的配套文档——书负责教你
> 怎么用这门语言；这份文档负责精确地、规范性地说清楚它相对 C++ 标准
> 到底改了什么。条款是逐步增补的；某个主题这里还没有对应条款，不代表
> 它没设计（书里可能已经讲过），只是还没在这份文档里正式化。
>
> English version: [en/README.md](../en/README.md)

## 目录

0. [前言部分（适用范围、规范性引用文件、术语和定义、一致性）](00-front-matter.md)
5. [`[[scpp::unsafe]]` Attribute](01-unsafe.md)
6. [所有权、初始化与 Move](02-ownership-and-move.md)
7. [解引用与成员访问](03-dereference-and-member-access.md)
8. [线程安全属性](04-thread-safety-properties.md)
9. [union 类型与 packed 布局](05-unions-and-packed-layout.md)
