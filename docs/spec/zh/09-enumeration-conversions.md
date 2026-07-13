# 14 枚举转换

## 14.1 枚举与整数之间的转换 [conv.enum.scpp]

(1) 除本小节明确修改的部分外，[dcl.enum]、[expr.static.cast]
和 [expr.cast] 原样适用于 SCPP26 程序里的枚举类型。

(2) 一个枚举类型的表达式，可以在且仅在对应转换在 C++26 中本来就合法时，
被显式转换成一个整数类型。这样的转换，不论写成 `static_cast`
（[expr.static.cast]）还是写成 explicit-type-conversion（[expr.cast]），
都被允许。

【注：`static_cast<int>(color)` 和 `int(color)` 都是 (2) 允许的转换例子。
——注释结束】

(3) 一个“源表达式是整数类型、目标类型是枚举类型”的显式转换，不合法
（ill-formed）。

【注：`static_cast<Color>(n)` 和 `Color(n)` 都会被拒绝。——注释结束】

(4) 如果程序需要把一个整数值转换成某个枚举类型，就必须调用
`scpp::enum_cast<T>(value)`，其中 `T` 指代该目标枚举类型。
`scpp::enum_cast<T>(value)` 必须返回
`std::expected<T, scpp::enum_cast_error>`。

(5) `scpp::enum_cast<T>(value)` 当且仅当 `value` 恰好等于 `T` 的某个枚举项
的底层整数值时成功。否则，它必须返回一个类型为
`scpp::enum_cast_error` 的错误值，其中 `invalid_value` 枚举项表示这类失败。
除这一条之外，不提供任何别的“从整数值到枚举类型”的未检查转换，也不提供
任何基于普通 cast 的转换路径。

---

[← 上一节：函数模板实参推导](08-function-template-argument-deduction.md) · [目录](README.md) · [下一节：迭代语句 →](10-iteration-statements.md)
