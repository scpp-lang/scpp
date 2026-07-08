# 8 线程安全属性

## 8.1 线程安全 attribute（Thread-safety attributes）[dcl.attr.scpp.thread]

(1) 本文档为每个类型定义两个布尔性质：**thread-movable** 和
**thread-shareable**。

(2) attribute-token `scpp::thread_movable` 或
`scpp::thread_shareable` 可以出现在一个 *attribute-specifier-seq*
（[dcl.attr.grammar]）里，并且附着于：

  (2.1) 一个参数声明；或者

  (2.2) 一个 class 或 struct 的声明。

(3) 如果 `[[scpp::thread_movable]]` 附着于一个参数声明，那么除非满足下列
条件之一，否则程序不合法（ill-formed）：

  (3.1) 如果这个参数的类型是到 `U` 的右值引用，那么 `U` 是
  thread-movable；否则

  (3.2) 在调用点为该参数确定出来的类型是 thread-movable。

(4) 如果 `[[scpp::thread_shareable]]` 附着于一个参数声明，那么除非满足下列
条件之一，否则程序不合法（ill-formed）：

  (4.1) 如果这个参数的类型是到 `U` 的右值引用，那么 `U` 是
  thread-shareable；否则

  (4.2) 在调用点为该参数确定出来的类型是 thread-shareable。

(5) 如果 `[[scpp::thread_movable]]` 附着于一个 class 或 struct 类型 `T`
的声明，那么不管结构化推导本来会为这个性质得出什么结果，`T` 都是
thread-movable。

(6) 如果 `[[scpp::thread_shareable]]` 附着于一个 class 或 struct 类型 `T`
的声明，那么不管结构化推导本来会为这个性质得出什么结果，`T` 都是
thread-shareable。

【注：用在参数形式时，这个 attribute 约束的是调用点处参数类型的使用；
用在 class/struct 形式时，它是对被声明类型本身的显式覆盖。——注释结束】

## 8.2 结构化推导（Structural derivation）[meta.thread.struct]

(1) 如果某个类型自己的 thread-movable 或 thread-shareable 值，不是由
8.1 或 8.4 里附着在该类型上的覆盖给出的，那么这个性质的值就由本小节的
规则按结构推导出来。

(2) 一个标量类型既是 thread-movable，也是 thread-shareable。

(3) 一个数组类型，当且仅当它的元素类型是 thread-movable 时，才是
thread-movable；当且仅当它的元素类型是 thread-shareable 时，才是
thread-shareable。

(4) 一个引用类型永远不是 thread-movable。

(5) 一个到 `T` 的引用，当且仅当同时满足下列条件时，才是
thread-shareable：

  (5.1) 它所引用到的类型是 `const T`；并且

  (5.2) `T` 是 thread-shareable。

(6) 一个指针类型既不是 thread-movable，也不是 thread-shareable。

【注：按 [§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe)，对一个
裸指针类型做解引用，本来就需要一个显式的 `[[scpp::unsafe]]` 语境。
本小节同样不会在缺少一个包裹类型上的显式覆盖时，默认赋予裸指针更强的
线程安全性质。——注释结束】

(7) 一个 class 或 struct 类型 `T`，当且仅当同时满足下列条件时，才是
thread-movable：

  (7.1) `T` 没有任何引用类型的非 static 数据成员；并且

  (7.2) `T` 的每个非 static 数据成员都是 thread-movable。

(8) 一个 class 或 struct 类型 `T`，当且仅当同时满足下列条件时，才是
thread-shareable：

  (8.1) `T` 的任何非 static 数据成员都不是 `mutable` 声明的；并且

  (8.2) `T` 的每个非 static 数据成员都是 thread-shareable。

【注：一个 `mutable` 成员只影响 thread-shareable；它本身不会阻止包含它
的类型成为 thread-movable。——注释结束】

(9) 一个闭包类型（[expr.prim.lambda.closure]），当且仅当同时满足下列条件
时，才是 thread-movable：

  (9.1) 它完全没有按左值引用 capture 的成员；并且

  (9.2) 每个按值 capture 的成员，其类型都是 thread-movable。

(10) 一个闭包类型（[expr.prim.lambda.closure]），当且仅当同时满足下列条件
时，才是 thread-shareable：

  (10.1) 它没有按可变左值引用 capture 的成员；

  (10.2) 每个按值 capture 的成员，其类型都是 thread-shareable；并且

  (10.3) 对每个按 `const` 左值引用 capture 的成员，它所引用到的类型都
  是 thread-shareable。

## 8.3 内置谓词（Builtin predicates）[expr.prim.scpp.thread]

(1) 形如 `scpp::is_thread_movable(T)` 和
`scpp::is_thread_shareable(T)` 的形式，是内置谓词。

(2) 在每个这种形式里，`T` 都必须命名一个类型。圆括号之间的 token 序列，
按一个类型操作数解析，而不是按一个普通函数调用里的表达式操作数解析。

(3) 每个这种形式都是一个类型为 `bool` 的 prvalue，并且可以出现在任何
允许布尔常量表达式的地方。

(4) `scpp::is_thread_movable(T)` 的求值结果，是 `T` 自己的
thread-movable 值，按以下顺序确定：

  (4.1) 如果 `T` 上存在 8.4 下的覆盖，就取那个值；

  (4.2) 否则，如果 `T` 上存在 8.1(5) 下的覆盖，就取那个值；或者

  (4.3) 否则，就取 8.2 为 `T` 结构化推导出来的结果。

(5) `scpp::is_thread_shareable(T)` 的求值结果，是 `T` 自己的
thread-shareable 值，按以下顺序确定：

  (5.1) 如果 `T` 上存在 8.4 下的覆盖，就取那个值；

  (5.2) 否则，如果 `T` 上存在 8.1(6) 下的覆盖，就取那个值；或者

  (5.3) 否则，就取 8.2 为 `T` 结构化推导出来的结果。

【注：这种语法对应的是编译器内置 trait（比如
`__is_trivially_copyable(T)`），不是一个带值实参的普通函数调用。——注释结束】

## 8.4 条件式覆盖（Conditional override）[dcl.attr.scpp.thread.if]

(1) attribute-token `scpp::thread_movable_if` 可以出现在一个
*attribute-specifier-seq*（[dcl.attr.grammar]）里，并且附着于一个 class
或者 struct 的声明。

(2) `[[scpp::thread_movable_if(a, b)]]` 恰好带两个实参。

(3) `a` 和 `b` 都必须是布尔常量表达式。

(4) 对一个非模板 class 或 struct，`a` 是该类型自己的 thread-movable 值，
`b` 是该类型自己的 thread-shareable 值。

(5) 对一个 class 或 struct 模板，`a` 和 `b` 都会在每次实例化时、用这次
实例化的模板实参替换进去之后分别求值；`a` 是这次实例化自己的
thread-movable 值，`b` 是这次实例化自己的 thread-shareable 值。

(6) 由 (4) 或者 (5) 确立的这两个值，会替换掉这个类型或者实例化本来会
根据字段结构为这两个性质推导出来的结果。

(7) 这个 attribute 是一个对任何用户声明的 class 或 struct 都可用的普通
attribute；一个类型的 thread-movable 或者 thread-shareable 值，不会由
任何被特殊对待的库类型名字决定。

【注：下面这个声明，让 `unique_ptr<T>` 的两个性质分别独立地跟随 `T`：

```cpp
template<typename T>
class [[scpp::thread_movable_if(
    scpp::is_thread_movable(T),
    scpp::is_thread_shareable(T)
)]] unique_ptr {
    // ...
};
```

下面这个声明，只有在 `T` 同时是 thread-movable 且 thread-shareable 时，
才让 `shared_ptr<T>` 也拥有这两个性质：

```cpp
template<typename T>
class [[scpp::thread_movable_if(
    scpp::is_thread_movable(T) && scpp::is_thread_shareable(T),
    scpp::is_thread_movable(T) && scpp::is_thread_shareable(T)
)]] shared_ptr {
    // ...
};
```

在这两个例子里，这个 attribute 的使用方式，都跟它在任何别的用户声明
class 模板上的用法完全一样。——注释结束】

---

[← 上一节：解引用与成员访问](03-dereference-and-member-access.md) · [目录](README.md) · [下一节：union 类型与 packed 布局 →](05-unions-and-packed-layout.md)
