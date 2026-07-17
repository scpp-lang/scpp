# 7 常量求值

## 7.1 总则（General）[expr.const]

(1) 除本条款明确修改的部分外，[expr.const] 原样适用于 SCPP26 程序。

(2) 凡是本文档或 C++ 标准要求其为常量表达式的表达式，在本条款中称为进行
**required constant evaluation（必需常量求值）**。

(3) 只有当为确定该表达式值而必须求值的每一个操作都属于 7.2 允许的范围，
并且没有求值 7.3 列出的任何操作时，required constant evaluation 才是
良构（well-formed）的。

(4) 符合标准的实现，必须按照目标程序自身的语义模型来执行 required
constant evaluation，包括目标程序的标量取值范围、浮点语义，以及指针模型。

【注：本条款只约束那些**必须**产出常量表达式结果的求值。一个出现在这种
语境之外的 `constexpr` 函数调用，除非 C++ 标准或本文档的其它条款另有要求，
否则仍然是一次普通的运行期计算。——注释结束】

## 7.2 支持的子集（Supported subset）[expr.const.scpp.support]

(1) 就本条款而言，当且仅当一个类型属于下列之一时，它是
**constexpr-compatible（可 constexpr 类型）**：

  (1.1) `bool`、任一字符类型、任一有符号或无符号整数类型、`float`、
  `double`，或者实现所提供的其它标准浮点类型；

  (1.2) 指针类型，但其取值仅限于 (3) 允许的那些值；

  (1.3) 元素类型本身是 constexpr-compatible 的数组类型；

  (1.4) 每个非 static 数据成员都为 constexpr-compatible 的 trivial
  `struct` 类型；或者

  (1.5) 满足 (2) 的 `class` 类型。

(2) 一个 `class` 类型满足本段，当且仅当同时满足下列条件：

  (2.1) 该 `class` 的每个非 static 数据成员都是
  constexpr-compatible；

  (2.2) 该 `class` 没有任何非 static 数据成员被声明为 `mutable`；

  (2.3) 任何其求值会被要求发生的构造函数，都被声明为 `constexpr`
  或 `consteval`；并且

  (2.4) 不存在任何一条求值路径需要执行用户自定义析构函数。

(3) 在 required constant evaluation 期间，一个指针值只有在属于下列之一时
才被允许：

  (3.1) 空指针值；

  (3.2) 指向一个字符串字面量对象某元素、或者其尾后元素的指针；或者

  (3.3) 指向某个静态存储期对象的子对象、元素、或者其尾后元素的指针；
  且该对象本身已完成 constant-initialized，并且在这次求值期间不会被修改。

(4) 在 required constant evaluation 期间，实现必须支持下列对象的形成与使用：

  (4.1) 字符串字面量对象；

  (4.2) 元素类型为 constexpr-compatible 的定长数组；以及

  (4.3) 类型为 `std::span<const T>` 的对象，其中 `T` 为
  constexpr-compatible，并且该 span 的范围完全落在 (4.1) 或 (4.2)
  所述对象之内。

(5) 在 required constant evaluation 期间，实现必须支持对下列构造求值：

  (5.1) block 语句；

  (5.2) 局部声明；

  (5.3) 赋值；

  (5.4) `if` 语句；

  (5.5) `while` 语句；

  (5.6) 经典 `for` 语句与 range-based `for` 语句；

  (5.7) `return` 语句；

  (5.8) 递归调用；以及

  (5.9) 模板代换之后形成的 pack-expanded 表达式与语句。

(6) 在 required constant evaluation 期间，实现必须支持：

  (6.1) 对 `constexpr` 函数与构造函数的调用；

  (6.2) 对 `consteval` 函数与构造函数的调用；

  (6.3) 对整数、字符、浮点操作数进行算术与比较运算；以及

  (6.4) 其操作数本身在其它方面都合法（well-formed）的
  `sizeof(type-id)`、`sizeof(expression)` 与 `alignof(type-id)` 查询。

## 7.3 不支持的操作（Unsupported operations）[expr.const.scpp.unsupported]

(1) 如果 required constant evaluation 会求值一个 `[[scpp::unsafe]]`
*compound-statement*，或者一个被 `[[scpp::unsafe]]` 所附着函数的函数体，
则程序不合法（ill-formed）。

(2) 如果 required constant evaluation 会求值下列任一项，则程序不合法
（ill-formed）：

  (2.1) 通过 `extern "C"` 或其它 foreign-function interface 发起的调用；

  (2.2) 一个对常量求值而言其定义不可用的调用，包括其编译期函数体没有被
  提供出来的 imported 定义；或者

  (2.3) 一个其被选中操作需要依赖纯运行期动态分派的调用。

(3) 如果 required constant evaluation 会求值 `new`、`delete`、任何其它
动态存储分配或释放，或者任何其正确性依赖动态生命周期管理的操作，则程序
不合法（ill-formed）。

(4) 如果 required constant evaluation 会对 union 成员做读写、通过裸指针
修改对象、求值一个 lambda-expression、抛出异常、执行输入输出、创建或同步
线程、访问环境或文件系统，或者执行任何要求运行用户自定义析构函数的操作，
则程序不合法（ill-formed）。

【注：本小节把 SCPP26 v1 的常量求值子集明确写出来。一个程序即使在 C++26
里本来可以做对应的常量求值，只要落在这个子集之外，当前也还不能因此被
接受。——注释结束】

## 7.4 必需常量求值的失败（Failure of required constant evaluation）[expr.const.scpp.fail]

(1) 如果 required constant evaluation 会调用一个既没有声明为
`constexpr`、也没有声明为 `consteval` 的函数，并且该调用也不属于
[expr.const] 另行允许的情形，则程序不合法（ill-formed）。

(2) 如果 required constant evaluation 因为会求值下列任一项而无法产出结果，
则程序不合法（ill-formed）：

  (2.1) 检查型算术运算中的算术溢出；

  (2.2) 右操作数为 0 的除法运算符或取模运算符；

  (2.3) 对数组、字符串字面量对象或 `std::span` 的越界访问；或者

  (2.4) 访问一个对常量求值而言不可用的值。

(3) 符合标准的实现，必须对下列项目施加有限上界：

  (3.1) 嵌套常量求值深度；

  (3.2) 常量求值总步数；以及

  (3.3) 在 required constant evaluation 期间对单个循环所执行的迭代次数。

(4) (3) 里的这些上界，不得小于 512 层嵌套求值、1,000,000 步总求值步数，
以及单个循环 262,144 次迭代。

---

[← 上一节：union 类型与 packed 布局](05-unions-and-packed-layout.md) · [目录](README.md) · [下一节：`constexpr` 与 `consteval` 说明符 →](07-constexpr-and-consteval.md)
