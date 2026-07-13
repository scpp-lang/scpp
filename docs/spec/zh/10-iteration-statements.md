# 8 迭代语句

## 8.1 总则 [stmt.iter]

(1) 除本条款明确修改的部分外，[stmt.iter]、[stmt.while]、[stmt.for]
和 [stmt.ranged] 原样适用于 SCPP26 程序里的迭代语句。

(2) SCPP26 中的迭代语句是下列之一：

  (2.1) `while` 语句；

  (2.2) 经典 `for` 语句，形式为
  `for ( init-statementopt ; conditionopt ; expressionopt ) statement`；
  或

  (2.3) range-based `for` 语句，形式为
  `for ( for-range-declaration : for-range-initializer ) statement`。

## 8.2 经典 `for` 语句 [stmt.for]

(1) 经典 `for` 语句的求值顺序与控制流，与 C++26 相同：

  (1.1) 若存在 `init-statement`，它恰好在第一次求值条件之前执行一次；

  (1.2) 若存在条件表达式，它会在每次迭代之前求值；若其结果为 `false`，
  则退出循环，且该次迭代的循环体不会执行；以及

  (1.3) 若存在迭代表达式，它会在每次完整执行完循环体之后求值；即使该次
  迭代通过 `continue` 结束，也同样如此。

(2) 若省略条件表达式，则它视为 `true`。

(3) 若 `init-statement` 是一个声明，那么它是一个受
[§6.1](02-ownership-and-move.md#61-显式初始化要求与零初始化required-initialization-and-zero-initializationdclinit)
约束的局部变量声明，包括该小节对“局部变量必须显式带初始化器”的要求。

(4) 若 `init-statement` 是一个声明，则它引入的名字在条件表达式、迭代表达式
和循环体中都处于作用域内，并在退出该 `for` 语句时离开作用域。

## 8.3 Range-based `for` 语句 [stmt.ranged]

(1) `for-range-declaration` 是一个局部变量声明，只是去掉了结尾的 `;`；
除本小节明确修改的部分外，它受
[§6.1](02-ownership-and-move.md#61-显式初始化要求与零初始化required-initialization-and-zero-initializationdclinit)
约束。它恰好声明一个循环变量。

(2) `for-range-initializer` 恰好求值一次。

(3) 在 SCPP26 v1 中，`for-range-initializer` 的类型必须是下列之一：

  (3.1) 定长数组类型；

  (3.2) `std::span<T>`；或

  (3.3) `std::span<const T>`。

(4) 一个 range-based `for` 语句，会按下标递增顺序遍历
`for-range-initializer` 的各个元素：从下标 `0` 开始，到最后一个合法下标
之后结束。

(5) 在每次迭代开始之前，循环变量都从当前元素完成一次初始化；其方式，恰好
等同于“用该元素去初始化一个同类型的普通声明”。

(6) 如果循环变量按值声明，那么它在每次迭代都会得到一个新的元素副本；对
这个循环变量赋值，不会修改底层数组元素或者 span 元素。

(7) 如果循环变量按引用声明，那么它引用的，就是当前这次迭代所选中的那个
底层元素；其行为与普通引用声明完全一样。

(8) 如果循环变量按引用声明，并且它的初始化值是通过某个仍然存活着的 mutable
引用或者 mutable `std::span<T>` 局部变量或参数取得的，那么这个绑定就是一个
受
[§6.2](02-ownership-and-move.md#62-所有权move-状态与-reborrowbasiclife)
(7)-(10) 约束的 reborrow。

```cpp
int values[3]{1, 2, 3};

for (int i = 2; i >= 0; i = i - 1) {
    values[i] = values[i] + 1;
}

for (auto& value : values) {
    value = value + 10;   // 修改底层数组元素
}

for (const auto& value : values) {
    // value = 0;         // 不合法
}

std::span<int> view{values};
for (auto& value : view) {
    value = value + 1;    // value 是对当前 span 元素的一次 reborrow
}
```

---

[← 上一节：枚举转换](09-enumeration-conversions.md) · [目录](README.md)
