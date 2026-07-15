# 7 解引用与成员访问

## 7.1 class 的解引用运算符（Class dereference operators）[over.deref]

(1) 一个 class 可以声明一个名为 `operator*` 的非 static 成员函数。

(2) 一个 `operator*` 的声明，除非同时满足下列条件，否则不合法
（ill-formed）：

  (2.1) 它没有参数；并且

  (2.2) 它声明的返回类型，对某个对象类型 `T` 来说，要么是"到 `T` 的左值
  引用"，要么是"到 `const T` 的左值引用"。

(3) 一个形如 `*E` 的表达式，如果 `E` 的类型是某个 class 类型 `C`，或者
是到某个 class 类型 `C` 的引用，那么它等价于一次成员函数调用：被调用者
是按普通成员访问和 `const` 限定规则，为 `E` 选出的 `C::operator*`，而
receiver 是 `E`。

【注：本条款没有另外引入任何独立的所有权、别名或者生命周期规则。如果
`C::operator*` 返回一个引用，那么这个引用受的，正是本文档别处已经施加给
"一次成员函数调用返回出来的引用"的那套同一规则。——注释结束】

(4) 一个形如 `*E` 的表达式，如果 `E` 的类型是指针类型，依然受
[expr.unary.op] 以及本文档对指针解引用已施加的现有要求约束，包括
[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) (5.1) 和 (6)。

## 7.2 箭头表达式（Arrow expressions）[expr.ref.scpp.arrow]

(1) 一个形如 `E1->E2` 的表达式，等价于 `(*E1).E2`。

(2) 除了 (1) 之外，本文档不提供任何独立的 `operator->` 语义。

【注：跟 C++ 标准那套单独的 `operator->` 协议不一样，本文档永远只做
(1) 这一层重写。如果 `E1` 的类型是 class 类型，并且 `*E1` 按
[§7.1](03-dereference-and-member-access.md#71-class-的解引用运算符class-dereference-operatorsoverderef)
是良定义的，那么 (1) 里的成员访问操作的，就是那个同样的结果。——注释结束】

```cpp
struct Box {
    int value{};
public:
    int& operator*() { return value; }
    const int& operator*() const { return value; }
};

int f(Box& b) {
    *b = 1;          // (3): equivalent to b.operator*() = 1;
    return *b;       // (3): equivalent to b.operator*();
}
```

---

[← 上一节：所有权、初始化与 Move](02-ownership-and-move.md) · [目录](README.md) · [下一节：线程安全属性 →](04-thread-safety-properties.md)
