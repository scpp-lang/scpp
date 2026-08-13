# 16 空指针类型

## 16.1 空指针字面量及其类型 [basic.fundamental.scpp.nullptr]

(1) 除本条明确修改的部分外，[lex.nullptr]、[basic.fundamental] 和
[conv.ptr] 原样适用于 SCPP26 程序。

(2) 关键字 `nullptr` 是一个表示空指针值的字面量。它的类型是
`nullptr_t`——一个与其他任何类型都不同的基本类型（fundamental type），
且只有一个值。

(3) `nullptr_t` 既可以写成 `nullptr_t`，也可以写成 `std::nullptr_t`；
两种写法指代同一个类型。在一个没有 import 任何模块的翻译单元里，实现
也必须接受这两种写法，并且必须能确定 `nullptr` 的类型。

【注：C++26 本身不给这个类型任何可直接书写的名字，只在库里提供
`std::nullptr_t` 这个 `decltype(nullptr)` 的别名。SCPP26 则直接给它一个
关键字写法，就像已经对 `size_t` 和 `ptrdiff_t` 所做的那样。
——注释结束】

(4) `nullptr_t` 可以出现在任何可以使用类型的位置，包括变量或非静态数据
成员的声明类型、参数类型和返回类型。由类型为 `nullptr_t` 的实参出发，
*placeholder type*（[dcl.spec.auto]）与函数模板实参推导
（[§13.1](08-function-template-argument-deduction.md#131-从函数调用进行推导-tempdeductcallscpp)）
都会推导出 `nullptr_t`。

(5) `nullptr_t` 不属于标量类型家族。从一个 `nullptr_t` 类型的 prvalue
到某个标量类型的 `static_cast` 不合法（ill-formed）。

【注：`static_cast<int>(nullptr)` 不合法。——注释结束】

## 16.2 转换 [conv.ptr.scpp]

(1) 一个 `nullptr_t` 类型的 prvalue 可以隐式转换到下列目标类型：

  (1.1) 任意指针类型，包括指向 `void` 的指针和指向 const 限定类型的
  指针；

  (1.2) 任意函数指针类型；

  (1.3) `nullptr_t`；或

  (1.4) 声明了唯一参数类型为 `nullptr_t` 的构造函数的类类型，按普通的
  构造函数重载决议进行。

(2) 从 `nullptr_t` 到其他任何类型的转换都不合法（ill-formed），无论是
隐式请求的还是通过显式 cast 请求的。特别地，`nullptr_t` 既不转换成
`bool`，也不转换成任何整数类型。

【注：(2) 与 C++26 不同：C++26 允许 `nullptr_t` 的 prvalue 在直接初始化
（direct-initialization）下转换成 `bool`。SCPP26 根本不提供“指针转
`bool`”这条转换——对指针 `p` 来说 `if (p)` 和 `static_cast<bool>(p)`
都不合法——所以单独为空指针字面量放行这条转换，只会让它比它所表示的
指针本身更容易转成 `bool`。而且 C++26 用来表达这条转换的唯一写法是带
圆括号的直接初始化 `bool b(nullptr)`，它在 SCPP26 里本来就不是合法
语法。——注释结束】

(3) (1) 中的各项转换，在每一个“把值交给一个已声明类型”的边界上都同样
适用：变量或非静态数据成员的初始化器、函数调用的实参、构造函数调用的
实参，以及 `return` 语句的操作数。

(4) 在相等比较中，`nullptr_t` 类型的操作数只与指针类型、函数指针类型
或 `nullptr_t` 类型的操作数相容，与其他任何类型的操作数都不相容。

【注：程序判断指针是否为空，写成比较式：`p == nullptr`。
——注释结束】

(5) `nullptr` 不指代任何对象。它不引入任何借用
（[§6.2](02-ownership-and-move.md#62-所有权move-状态与-reborrow-basiclife)），
也不是生命周期来源（lifetime source）；由它初始化而来的值不会因此被
判定为悬垂。

## 16.3 标准库类型 [conv.ptr.scpp.lib]

(1) `std::unique_ptr<T>` 和 `std::shared_ptr<T>` 各自声明了一个唯一
参数类型为 `nullptr_t` 的构造函数。它构造出一个不拥有任何对象的空智能
指针；`std::unique_ptr<T> p = nullptr;` 与 `std::unique_ptr<T> p{};`
效果相同。

(2) `std::optional<T>` 没有声明这样的构造函数。
`std::optional<T> o = nullptr;` 不合法（ill-formed）。

【注：C++26 的 `std::optional` 接受的是 `std::nullopt_t`，同样的初始化
在那边也不合法。——注释结束】

---

[← 上一节：模块与命名空间](12-modules-and-namespaces.md) · [目录](README.md)
