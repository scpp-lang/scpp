# 16 空指针转换

## 16.1 空指针字面量及其类型 [conv.ptr.scpp]

(1) 除本小节明确修改的部分外，[lex.nullptr]、[conv.ptr] 和
[basic.fundamental] 原样适用于 SCPP26 程序。

(2) 关键字 `nullptr` 表示**空指针字面量**。它的类型是 `nullptr_t`，
这是一个独立的基本类型（fundamental type）。

(3) 不论是否 import 了任何模块，实现都认识 `nullptr_t`。它既可以写成
`nullptr_t`，也可以写成 `std::nullptr_t`；两种写法指代同一个类型。

【注：C++26 本身不给这个类型任何可直接书写的名字，只在库里提供
`std::nullptr_t` 这个 `decltype(nullptr)` 的别名。SCPP26 则直接给它一个
关键字写法——就像已经对 `size_t` 和 `ptrdiff_t` 所做的那样——这样一个
完全不 import 任何模块的程序也能写出这个类型名。——注释结束】

(4) `nullptr_t` 只有一个值，即空指针值。两个 `nullptr_t` 类型的 prvalue
相等。

## 16.2 转换 [conv.ptr.scpp.conv]

(1) 一个 `nullptr_t` 类型的 prvalue 可以隐式转换成：

  (1.1) 任意指针类型，包括指向 `void` 的指针和指向 const 限定类型的指针；

  (1.2) 任意函数指针类型；

  (1.3) `nullptr_t` 自身；以及

  (1.4) 某个类类型——当该类声明了唯一参数类型为 `nullptr_t` 的构造函数
  时，按普通的构造函数重载决议进行。

(2) 从 `nullptr_t` 到其他任何类型的转换都不合法（ill-formed）。特别地，
`nullptr_t` 不转换成 `bool`，也不转换成任何整数类型，无论是隐式转换还是
显式 cast。

【注：这一点与 C++26 不同：C++26 允许 `nullptr_t` 的 prvalue 在
直接初始化（direct-initialization）下转换成 `bool`。SCPP26 根本不存在
“指针转 `bool`”这条转换（见标量转换规则对 [conv.bool] 的修改）：对指针
`p` 来说 `if (p)` 不合法，`static_cast<bool>(p)` 同样不合法。如果单独放行
`bool b(nullptr)`，就会出现“空指针字面量比它所要表示的指针本身更容易转成
`bool`”这种颠倒的局面；何况 C++26 用来表达它的那个写法——带圆括号的直接
初始化——在 SCPP26 里本来就不是合法语法。程序判断指针是否为空，写成比较
式：`p == nullptr`。——注释结束】

(3) (1) 中的各项转换，在每一个“把值交给一个已声明类型”的边界上都同样
适用：变量的初始化器、函数调用的实参、构造函数调用的实参，以及 `return`
语句的操作数。

(4) 相等运算符 `==` 和 `!=` 接受“一侧是 `nullptr_t`、另一侧是指针类型、
函数指针类型或 `nullptr_t`”的操作数组合。除此之外，没有别的运算符接受
`nullptr_t` 操作数。

## 16.3 所有权与生命周期 [conv.ptr.scpp.own]

(1) 空指针字面量不指代任何对象。因此它不引入任何借用；由它初始化而来的
值没有生命周期来源（lifetime source），一个取值来自 `nullptr` 的引用或
指针，永远不会因此被判定为悬垂。

(2) 通过 (1.4) 由 `nullptr` 构造出来的类类型对象是已初始化的，并且和该
类型的其他对象一样，在生命周期结束时被析构。

---

[← 上一节：模块与命名空间](12-modules-and-namespaces.md) · [目录](README.md)
