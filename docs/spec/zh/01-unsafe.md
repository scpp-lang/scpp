# 5 `unsafe` 复合语句

## 5.1 关键字（Keyword）[lex.key]

(1) `unsafe` 是一个关键字。它在任何上下文里都是保留的；不能被用作
标识符。

【注：本文档没有把 `unsafe` 加成 C++26 `override`、`final`、
`import`、`module`（[lex.name]）那种上下文相关（context-sensitive）
标识符——一个程序如果把 `unsafe` 用作某个实体的名字，不管这个实体
本身合不合法，都是不合法（ill-formed）的。——注释结束】

## 5.2 语句（Statement）[stmt.unsafe]

(1) 语法：

```
statement:
    ...
    unsafe-compound-statement

unsafe-compound-statement:
    unsafe compound-statement
```

(2) *unsafe-compound-statement* 的 *compound-statement*，是一个普通的
*compound-statement*（[stmt.block]）：它跟任何别的 *compound-statement*
一样引入一个块作用域，块内声明的每个名字都遵循跟别的块一样的作用域
规则。

【注：*unsafe-compound-statement* 本身不是一种独立的作用域种类。本
文档没有给 `unsafe { }` 任何跟普通 `{ }` 复合语句不一样的作用域行为。
——注释结束】

(3) *unsafe-compound-statement* 的 *compound-statement*，是一个
unsafe context（[§3.3](00-front-matter.md#3-术语和定义terms-and-definitions)）；程序里别的任何位置都是 safe
context（[§3.2](00-front-matter.md#3-术语和定义terms-and-definitions)）。

(4) *unsafe-compound-statement* 可以在词法上嵌套在另一个
*unsafe-compound-statement* 的 *compound-statement* 里面。这种嵌套
没有额外效果：嵌套在里面的和外面包着的这两个 *compound-statement*，
按 (3) 本来就都已经各自独立地是 unsafe context 了。

(5) 下列是 gated operation（[§3.4](00-front-matter.md#3-术语和定义terms-and-definitions)）：

  (5.1) 对指针类型的值做间接寻址（indirection），或者做指针算术
  （[expr.unary.op]、[expr.add]）；

  (5.2) `reinterpret_cast`（[expr.reinterpret.cast]），以及两个指针
  类型之间、且这两个类型互相都不能靠本文档允许的隐式转换互相转换时，
  做的任何 *explicit-type-conversion*（[expr.cast]）；

  (5.3) 访问一个不是 tagged union 的 union 的非 static 数据成员
  （SCPP26 的"tagged union"由未来的某个条款定义；在那样的条款加入
  本文档之前，为此处的目的，每个 union 都当作 untagged 处理）
  （[class.union]）；

  (5.4) 一个 *new-expression* 或者 *delete-expression*（[expr.new]、
  [expr.delete]）；

  (5.5) 对一个 static 或者 thread 存储期、且不是 const 限定的变量，
  做左值到右值转换（lvalue-to-rvalue conversion），或者对它赋值
  （[basic.stc.static]、[basic.stc.thread]）；

  (5.6) 调用一个 *postfix-expression* 所指代的、声明为 C 语言链接
  的函数（[dcl.link]）。

(6) 除非本文档明确另有声明，一个 gated operation（5），在 safe
context 里不合法（ill-formed），在 unsafe context 里合法
（well-formed）。

(7) 本文档在别的条款（不止这一条）里，对程序施加各种要求——包括但
不限于关于所有权（ownership）、别名（aliasing）、生命周期
（lifetime）、算术溢出（arithmetic overflow）的要求。除非那个条款
明确另有声明，那个条款的要求，不管它所约束的构造出现在 safe context
还是 unsafe context 里，都同样适用：一个 *unsafe-compound-statement*，
只放宽 (6) 里针对 (5) 列举的这些 gated operation 的不合法性，别的
一概不放宽。

【注：具体来说，如果某个未来条款要求实现对某项操作执行运行时检查
（比如对算术溢出、或者对下标越界的检查），那个条款可能会（跟 (6)
不一样）额外允许实现在 unsafe context 里跳过这项检查本身，但仍然要求
被检查的这个操作在任何上下文里都合法。跳过这样一项检查，是一项独立
于 (6) 的合法性规则、单独授予的许可（如果真的授予的话），是由引入
这项检查的那个条款授予的，不是本条款授予的。——注释结束】

---

[← 上一节：前言部分](00-front-matter.md) · [目录](README.md)
