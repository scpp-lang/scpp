# 16 基本类型与标准转换

## 16.1 标量类型 [basic.fundamental.scpp]

(1) 本条规定 SCPP26 的*标量类型*和*空指针类型*，以及它们与其他任何
类型之间的转换。就这些类型而言，本条取代 [basic.fundamental] 与
[conv]。特别地，[conv.prom]、[conv.integral]、[conv.fpprom]、
[conv.double]、[conv.fpint] 和 [conv.bool] 不适用于 SCPP26 程序，
[expr.arith.conv] 不施加于任何运算符的操作数；§1(2) 也不会把它们重新
引入。

(2) 标量类型就是表 1 中列出的那些类型，没有别的。表 1 中的每个名字都
指代一个互不相同的类型。表 1 中没有任何一个名字是另一个名字的别名；
表 1 中两个位宽相同的类型也不是同一个类型。

表 1 —— 标量类型

| 类型 | 大小（字节） | 说明 |
|---|---|---|
| `bool` | 1 | 只有 `false` 和 `true` 两个值，位模式分别为 0 和 1。 |
| `char` | 1 | 一个字节值，与 `int8_t`、`uint8_t` 以及其他任何类型都不同。 |
| `int8_t`、`int16_t`、`int32_t`、`int64_t` | 1、2、4、8 | 位宽恰好如上的有符号整数。 |
| `uint8_t`、`uint16_t`、`uint32_t`、`uint64_t` | 1、2、4、8 | 位宽恰好如上的无符号整数。 |
| `int` | 4 | 32 位有符号整数。 |
| `long` | 8 | 64 位有符号整数。 |
| `unsigned int` | 4 | 32 位无符号整数。 |
| `unsigned long` | 8 | 64 位无符号整数。 |
| `float32_t`、`float64_t` | 4、8 | IEEE-754 binary32 与 binary64。 |
| `float` | 4 | IEEE-754 binary32。 |
| `double` | 8 | IEEE-754 binary64。 |
| `size_t` | 指针宽度 | 位宽等于目标平台指针宽度的无符号整数。 |
| `ptrdiff_t` | 指针宽度 | 位宽等于目标平台指针宽度的有符号整数。 |

(3) 标量类型的对齐要求等于它的大小。实现必须在任何目标平台上都提供
表 1 中的每一个类型。`size_t` 和 `ptrdiff_t` 是仅有的大小随目标平台
变化的标量类型。

【注：`long` 尤其在任何目标平台上都是同一个位宽，这与 C++26 不同——
在 C++26 里它在 LP64 下是 64 位、在 LLP64 下是 32 位。——注释结束】

(4) 下列 C++26 基本类型不提供，其名字也不指代任何类型：`short`、
`long long`、`signed char`、`unsigned char`、`long double`、
`wchar_t`、`char8_t`、`char16_t`、`char32_t`。名字 `int128_t` 和
`uint128_t` 也不指代任何类型。

(5) `unsigned` 本身不是类型说明符（type-specifier）。在类型中，
`unsigned` 后面必须紧跟 `int` 或 `long`；否则程序不合法
（ill-formed）。

## 16.2 标量类型的字面量 [lex.literal.scpp]

(1) *integer-literal* 和 *floating-point-literal* 自身没有类型。这样
的字面量取它所处上下文所要求的那个标量类型：它所初始化或被赋值给的
实体的声明类型、它作为实参所对应的形参类型、以它为 `return` 语句操作数
的那个函数的返回类型，或者二元运算符另一侧操作数、条件表达式另一分支
的类型。

(2) integer-literal 可以取表 1 中除 `bool` 和 `char` 以外的任何类型。
floating-point-literal 可以取 `float`、`float32_t`、`double` 或
`float64_t`。

(3) 若没有任何上下文确定其类型，则 integer-literal 的类型是 `int`，
floating-point-literal 的类型是 `double`。*placeholder type*
（[dcl.spec.auto]）不构成这样的上下文。

(4) `true` 和 `false` 的类型是 `bool`。*character-literal* 的类型是
`char`。

【注：`char c = 65;` 和 `bool b = 1;` 不合法；`char c = 'A';`、
`bool b = true;` 和 `char c = static_cast<char>(65);` 合法。
`auto x = 5;` 声明的是 `int`，`auto y = 1.5;` 声明的是 `double`。
——注释结束】

## 16.3 标量类型之间的转换 [conv.scpp]

(1) 任何两个不同的标量类型之间都不存在隐式转换。凡是要求某个标量类型
的值的位置，该值必须恰好就是那个类型。这适用于

  (1.1) 变量或非静态数据成员的初始化器；

  (1.2) 赋值的右操作数；

  (1.3) 函数调用或构造函数调用的实参；

  (1.4) `return` 语句的操作数；

  (1.5) 二元运算符的两个操作数；以及

  (1.6) 条件表达式的两个分支。

在上述任一位置上出现另一个标量类型的值，都使程序不合法（ill-formed）。
§16.2 先行适用：字面量操作数是直接取得所要求的类型，而不是被转换成
它。

【注：无论两个类型之间的差别多小，也无论该转换是否保值，这条规则都
成立。`int8_t` 到 `int16_t`、`int32_t` 到 `float64_t`、
`unsigned int` 到 `long` 都不合法。对两个位宽相同的类型同样成立，
例如 `int` 与 `int32_t`、`float` 与 `float32_t`——§16.1(2) 已经规定
它们是不同的类型。——注释结束】

(2) 标量类型之间的转换以显式方式请求，写作 `static_cast<T>(expr)` 或
`(T)expr`，其中 `T` 是标量类型、`expr` 是标量类型的表达式。任何这样的
转换都合法。所得的值由 [conv.integral]、[conv.double]、[conv.fpint]
和 [conv.bool] 规定。

(3) 在重载决议中，标量类型的实参只有在与形参类型完全相同时才匹配；
对这样的实参，除恒等转换外不存在任何隐式转换序列
（[over.best.ics]）。

【注：因此匹配是精确匹配，调用也就不可能因标量类型之间的某个转换而
产生二义性。——注释结束】

(4) 不存在为了得到 `bool` 而施加于某个值的转换。选择语句的条件、
迭代语句的条件、条件表达式的条件、`!` 的操作数，以及 `&&` 和 `||`
的每个操作数，都必须本来就是 `bool` 类型。

【注：对标量 `x` 来说，`if (x)` 不合法，`if (static_cast<bool>(x))`
合法。对指针 `p` 来说，`if (p)` 和 `static_cast<bool>(p)` 都不合法：
SCPP26 根本不提供“指针转 `bool`”这条转换。这与 [conv.bool] 不同——
在 C++26 里，任何算术类型、无作用域枚举类型、指针类型和成员指针类型
都可以按语境转换成 `bool`。——注释结束】

(5) 标量类型与指针类型、函数指针类型或 `nullptr_t` 之间，不存在任何
转换，无论隐式还是显式。

## 16.4 空指针字面量及其类型 [basic.fundamental.scpp.nullptr]

(1) 除本子条明确修改的部分外，[lex.nullptr] 原样适用于 SCPP26 程序。

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

(5) `nullptr_t` 不是标量类型：它不在表 1 之列，§16.3 对它不适用。从一个
`nullptr_t` 类型的 prvalue 到某个标量类型的 `static_cast` 不合法
（ill-formed）。

【注：`static_cast<int>(nullptr)` 不合法。——注释结束】

## 16.5 从空指针类型出发的转换 [conv.ptr.scpp]

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
`bool`”这条转换（§16.3(4)），所以单独为空指针字面量放行这条转换，
只会让它比它所表示的指针本身更容易转成 `bool`。而且 C++26 用来表达
这条转换的唯一写法是带圆括号的直接初始化 `bool b(nullptr)`，它在
SCPP26 里本来就不是合法语法。——注释结束】

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

## 16.6 标准库中的空指针类型 [conv.ptr.scpp.lib]

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
