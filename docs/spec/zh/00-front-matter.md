# SCPP26 语言标准

**状态**：工作草案。尚不完整——条款是逐步增补的；某个主题现在还没有对应
条款，不代表 SCPP26 对这个主题没有规则，只是还没在这份文档里正式写出来
（参见[那本书](../../book/zh/README.md)里尚未在此正式化的主题）。

---

## 1 适用范围（Scope）

(1) 本文档规定了用 SCPP26 编程语言编写的程序的形式，并确立了对它们的
解释方式。

(2) SCPP26 的定义是：以 ISO/IEC 14882:2026《*Programming languages —
C++*》（以下称"C++ 标准"或"C++26"）为基础，按照下列各条款修改而成。
除非本文档后续条款明确修改，C++ 标准的每一条要求原样适用于 SCPP26
程序，不做改动。

(3) 本文档只用**差异**来刻画 SCPP26 跟 C++ 标准的区别：它引入的极少量
新语法形式（第一个见第 5 条）、它对 C++ 标准本来会无条件接受的程序
施加的额外语义限制和静态检查，以及——只在后续条款明确声明的情况下——
C++ 标准允许、但本文档不允许的构造。

(4) 符合本文档，要求同时符合本文档修改之后的 C++ 标准；本文档本身
并不单独构成一门完整的编程语言。

## 2 规范性引用文件（Normative references）

(1) 下列引用文件是应用本文档不可或缺的：ISO/IEC 14882:2026《*Programming
languages — C++*》。

(2) 凡是不注明日期的引用文件，其最新版本（包括所有修改单）适用于本
文档。

## 3 术语和定义（Terms and definitions）

就本文档而言，C++ 标准给出的术语和定义适用，另外还有下列术语和定义。

**3.1 erasure（可擦除变换）**
对一份 SCPP26 翻译单元做源码层面的变换：移除每一个 *attribute-namespace*
（[dcl.attr.grammar]）是 `scpp` 的 attribute。

【注：第 4 条要求这个变换的结果必须是一份良构（well-formed）的
C++26 翻译单元。因为每一个 SCPP26 专有构造都拼写成 `scpp`
*attribute-namespace* 下的一个 attribute——包括
`[[scpp::unsafe]]`（[第 5 条](01-unsafe.md#51-attribute属性dclattrscppunsafe)）
——所以这个变换只有这一步；本文档没有引入任何真正的 C++26 编译器
不能原样解析并直接忽略的关键字、运算符或者别的记号（另见第 4 条 (2)
关于 `-Wno-unknown-attributes` 的注）。——注释结束】

**3.2 safe context（安全上下文）**
SCPP26 程序里，不属于 unsafe context（3.3）的任何位置。

**3.3 unsafe context（不安全上下文）**
下列两者之一：
  (3.3.1) 一个被带有含 attribute-token `unsafe`（位于 `scpp`
  *attribute-namespace* 下）的 *attribute-specifier-seq* 所附着的
  *compound-statement*（[第 5 条](01-unsafe.md#51-attribute属性dclattrscppunsafe)），
  连同被它在词法上包住的每一个位置；或者
  (3.3.2) 一个被带有含 attribute-token `unsafe`（位于 `scpp`
  *attribute-namespace* 下）的 *attribute-specifier-seq* 所附着的函数的
  整个 *function-body*（[dcl.fct.def.general]）（[第 5 条](01-unsafe.md#51-attribute属性dclattrscppunsafe)），
  连同被它在词法上包住的每一个位置。

**3.4 gated operation（受管制操作）**
本文档认定的、在 safe context（3.2）里不合法（ill-formed）、在
unsafe context（3.3）里合法（well-formed）的操作。

## 4 一致性（Conformance）

(1) 一个符合本标准的实现，对每一份在本文档、或者本文档修改之后的
C++ 标准之下不合法（ill-formed）的 SCPP26 翻译单元，必须发出至少一条
诊断信息——除非本文档明确允许某个具体的不合法构造可以不发诊断。

(2) 一个符合本标准的实现，必须把任何一份合法（well-formed）SCPP26
翻译单元的 erasure（3.1）结果，当成一份合法的 C++26 翻译单元接受。

【注：因为 erasure（3.1）永远只是移除 `scpp` 命名空间下的 attribute，
而 C++26 编译器本来就会原样接受一个它不认识的 attribute
（[dcl.attr.grammar]），所以一份合法的 SCPP26 翻译单元，在 erasure
之前，其实就已经能被一个符合标准的 C++26 实现接受了——通常只会触发
一条"未知 attribute"的诊断信息，实现自己的选项（比如类似
`-Wno-unknown-attributes` 这样的命令行选项）可以用来关掉这条诊断。
erasure（3.1）仍然是本文档要求一个符合标准的实现必须接受的唯一变换；
它的作用是产出一份不带这类诊断的输出，而不是让翻译单元变得"能编译"——
它本来就能编译。——注释结束】

【注：本文档可能会要求实现执行一项额外的运行时检查（比如某个后续
条款规定的算术溢出检查），而 C++26 编译同一份 erasure 之后的文本时
不需要做这项检查；因此，会触发这种检查的执行，两者的可观察行为不需要
一致。——注释结束】

---

[目录](README.md) · [下一节：`[[scpp::unsafe]]` Attribute →](01-unsafe.md)
