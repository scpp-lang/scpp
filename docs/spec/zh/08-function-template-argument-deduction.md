# 13 函数模板实参推导

## 13.1 从函数调用进行推导 [temp.deduct.call.scpp]

(1) 除本小节明确修改的部分外，[temp.deduct.call] 原样适用于 SCPP26
程序里从函数调用进行的模板实参推导。

(2) 在一个函数形参列表中，函数参数包只能作为该列表里的最后一个参数声明出现。

(3) 从函数调用进行的推导，是针对整个函数形参列表完成的。一个较早函数形参
与其对应实参之间的兼容性，在所有较晚、且不是 defaulted 的函数形参都被
纳入推导之前，不得被最终确定。

(4) 如果某个函数形参 *P* 的类型包含一个模板参数包 `Args...`，而这个参数包
是在一个或多个较晚函数形参处才第一次被直接推导出来，那么实现必须：

  (4.1) 先根据这些较晚的推导来源，确定 `Args...` 的最终绑定；

  (4.2) 再把该绑定代换进 *P*；并且

  (4.3) 最后再判断对应实参是否满足经此代换后的 *P*。

(5) 如果 (4.2) 中经代换后的类型不能被对应实参满足，则程序不合法
（ill-formed）。

(6) 本小节不会据此允许一个“非最后位置的函数参数包”；即使按 (4) 的方式
进行推导，(2) 仍然适用。

## 13.2 `requires`-表达式中的复合要求 [expr.prim.req.compound.scpp]

(1) 本小节适用于出现在 *requires-expression*（[expr.prim.req]）中的
*compound-requirement*（[expr.prim.req.compound]）。

(2) 除本小节明确修改的部分外，SCPP26 程序中的 *compound-requirement*
受普通 C++ 规则支配。

(3) 对于形如 `{ *E* }` 的不带限定的 *compound-requirement*，如果其闭合
`}` 之后没有 *type-constraint*，且前面没有前置的 `noexcept`，则表达式
*E* 必须是下列形式之一：

  (3.1) 一个函数调用表达式，其 *postfix-expression* 指代某个函数或函数
  对象；

  (3.2) 一个成员函数调用表达式，其 *postfix-expression* 指代一次类成员
  访问，并选中被调用的成员函数；或

  (3.3) 一个构造具名类型对象的 direct-initialization 或
  list-initialization 表达式，写作 `T(args...)` 或 `T{args...}`，其中
  `T` 指名该类型。

(4) 如果 *E* 不属于 (3) 列出的形式之一，则程序不合法（ill-formed）。

(5) 如果某个 *compound-requirement* 的表达式属于 (3.3) 的形式，那么当且
仅当对于外围 *requires-expression* 的探测参数与操作数所确定的类型而言，
该具名类型能够按所写形式由这些实参类型构造时，该要求才被满足。

(6) (5) 中的满足性检查是结构性的。它作为判定该定义中所写要求是否良构并对
被探测类型成立的一部分来执行；它不要求存在任何用于表示“可构造性”的
特殊编译器内建 concept 名称。

[Note: 这允许一个 concept 直接表达“可复制构造”，例如
`requires(T t) { T{t}; }`；更一般地，也允许通过
`requires(T t, U u) { T(t, u); }` 这样的形式表达“可由任意实参类型构造”。
其含义与普通 C++20 在复合要求里使用构造表达式的做法一致，例如用于定义
`copy_constructible` 或 `constructible_from` 的那些写法。— end note]

---

[← 上一节：`constexpr` 与 `consteval` 说明符](07-constexpr-and-consteval.md) · [目录](README.md) · [下一节：枚举转换 →](09-enumeration-conversions.md)
