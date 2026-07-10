# 9 `constexpr` 与 `consteval` 说明符

## 9.1 `constexpr` 与 `consteval` 说明符 [dcl.constexpr]

(1) 除本条款明确修改的部分外，[dcl.constexpr] 原样适用于 SCPP26 程序。

(2) 记号 `constexpr` 与 `consteval` 具有 C++ 标准赋予它们的含义；本文档
没有为“可在编译期求值的声明”引入任何额外拼写。

(3) 被声明为 `constexpr` 的函数或构造函数，具有参与编译期求值的资格。
如果对此类函数或构造函数的一次调用发生在 required constant evaluation
（7.1）中，那么这次调用必须满足第 7 条。否则，这次调用就是一次具有 C++
标准原有语义的普通运行期调用。

(4) 被声明为 `consteval` 的函数或构造函数，是 immediate function。对它的
每一次 potentially-evaluated 调用，都必须产出一个常量表达式；否则程序
不合法（ill-formed）。

(5) 析构函数、分配函数或释放函数，不得声明为 `consteval`。

(6) 一个 `constexpr` 或 `consteval` 构造函数，只有在其类类型满足 7.2(2)
时，才可以参与 required constant evaluation。

【注：本条款直接复用 C++ 的 `constexpr` / `consteval` 拼写和含义。
SCPP26 的专有限制，体现在第 7 条所规定的常量求值支持子集。——注释结束】

## 9.2 `if consteval` 语句 [stmt.if]

(1) 形如 `if consteval` 的 `if` 语句，是一个 **consteval if
statement**。形如 `if !consteval` 的 `if` 语句，是一个
**negated consteval if statement**。

(2) 除本小节明确修改的部分外，[stmt.if] 原样适用于 consteval if
statement 与 negated consteval if statement。

(3) 如果一个 consteval if statement 在一个 manifestly
constant-evaluated（[expr.const.defns]）的上下文中被求值，那么它的第一个
substatement 会被执行。这个第一个 substatement 是一个 immediate
function context。

(4) 否则，如果一个 consteval if statement 带有 else 部分，那么它的第二个
substatement 会被执行。

(5) 如果一个 negated consteval if statement 在一个 manifestly
constant-evaluated（[expr.const.defns]）的上下文中被求值，那么它的第一个
substatement 不会被执行；而如果带有 else 部分，则第二个 substatement
会被执行。

(6) 否则，negated consteval if statement 的第一个 substatement 会被执行。

(7) consteval if statement 或 negated consteval if statement 的每一个
substatement，都是一个 control-flow-limited statement（[stmt.label]）。

---

[← 上一节：常量求值](06-constant-evaluation.md) · [目录](README.md) · [下一节：函数模板实参推导 →](08-function-template-argument-deduction.md)
