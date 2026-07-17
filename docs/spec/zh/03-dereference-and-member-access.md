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
"一次成员函数调用返回出来的引用"的那套同一规则。尤其是：如果那个引用是从
receiver 导出的，那么经由 receiver 的隐式对象参数形成的任何 borrow 或
reborrow，仍然受
[§6.2](02-ownership-and-move.md#62-所有权move-状态与-reborrowbasiclife)
(7)-(12) 与 (23) 约束。——注释结束】

(4) 一个形如 `*E` 的表达式，如果 `E` 的类型是指针类型，依然受
[expr.unary.op] 以及本文档对指针解引用已施加的现有要求约束，包括
[§5.1](01-unsafe.md#51-scppunsafe-attribute) (5.1) 和 (6)。

## 7.2 class 的箭头运算符（Class arrow operators）[over.ref]

(1) 一个 class 可以声明一个名为 `operator->` 的非 static 成员函数。

(2) 一个 `operator->` 的声明，除非同时满足下列条件，否则不合法
（ill-formed）：

  (2.1) 它没有参数；并且

  (2.2) 它声明的返回类型，要么是指针类型，要么是 class 类型，
  要么是到 class 的引用类型。

(3) 一个用户写出的、形如 `E.operator->()` 的表达式，就是一次普通的成员函数
调用。它产生的，就是那次调用声明出来的结果；它本身不会触发 §7.3 里的那个
特殊箭头表达式协议。

【注：因此，`auto raw = p.operator->();` 暴露出来的，就是一个普通值，其类型
就是 `operator->` 真正返回的类型。如果那个值是裸指针，那么之后对 `raw`
的使用，仍然全部受普通指针规则约束；§7.3 里对 `E1->E2` 的“安全 carve-out”
并不会套用到这里。——注释结束】

## 7.3 箭头表达式（Arrow expressions）[expr.ref.scpp.arrow]

(1) 一个形如 `E1->E2` 的表达式，如果 `E1` 的类型是指针类型，那么它等价于
`(*E1).E2`。

(2) 一个形如 `E1->E2` 的表达式，如果 `E1` 的类型是某个 class 类型 `C`，
或者是到某个 class 类型 `C` 的引用，那么按下面的规则解析：

  (2.1) 如果 overload resolution 为 `E1` 选中了某个成员 `C::operator->`，
  实现就必须先求值那次调用，并检查它的结果。

  (2.2) 如果那个结果的类型是指针类型，那么 `E1->E2` 就通过“经由该指针做一次
  成员访问”来完成。

  (2.3) 如果那个结果的类型是某个 class 类型 `D`，或者是到某个 class 类型
  `D` 的引用，并且 overload resolution 又为那个结果选中了某个成员
  `D::operator->`，那么实现就要对这个新结果再次应用 (2.1)-(2.3)。

  (2.4) 否则，程序不合法（ill-formed）。

如果在 (2.4) 下程序不合法，那就意味着：某一步被选中的 `operator->` 结果，既
不是指针，也不是一个还能继续做下一步 `operator->` 的 class / 引用到 class 值。
实现应当把这类错误诊断成“`operator->` 链最终没有得到指针”。

(3) 如果 `E1` 的类型是 class 类型或者到 class 的引用类型，但按 (2.1) 没有为它
选中任何 `operator->`，那么程序不合法（ill-formed）。

【注：跟本文档此前已经 shipped 的实现行为不一样，SCPP26 不再给“只是定义了
`operator*` 的 class 类型”提供任何从 `E1->E2` 到 `(*E1).E2` 的 blanket
fallback。这里要跟真实 C++ 完全对齐：class 类型上的 `->` 必须显式定义
`operator->`。因此，任何想支持 `->` 的现有库 wrapper——包括 `std::unique_ptr`
——都需要在后续迁移里显式补上 `operator->` 声明。——注释结束】

(4) 如果同一个 class 同时提供了 `operator*` 和 `operator->`，那么 `E1->E2`
使用的是 `operator->`，并受 (2) 约束；`operator->` 的存在不会改变 `*E1`
的含义，后者仍然受 §7.1 约束。

(5) 就本小节而言，一次被选中的 `operator->` 调用，如果它的声明符带有
`[[scpp::lifetime(name)]]`，并且那个注解按
[§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
(23) 把结果绑定到了该次调用的隐式对象参数上，那么这一步调用就叫做
**receiver-tied**。

(6) 一个 receiver-tied 注解只约束生命周期本身。它本身并不能证明
`operator->` 产出的那个裸指针值一定有效，也不会放宽
[§5.1](01-unsafe.md#51-scppunsafe-attribute) 或
[§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
(21) 对裸指针解引用施加的普通 `[[scpp::unsafe]]` 要求。

由于 [§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
只允许在“返回引用 / 指针 / span”的位置上使用 `[[scpp::lifetime(name)]]`，
所以一个返回 class prvalue 的 `operator->` 虽然可以参加 (2) 的链式协议，
但那一步并不是 receiver-tied。

(7) `E1->E2` 的安全情形，复用了 §7.1 里的 class `operator*` 再加上
[§6.2](02-ownership-and-move.md#62-所有权move-状态与-reborrowbasiclife)
(7)-(12) 与 (23) 已经在使用的、那套“以 receiver 为根”的 borrow 纪律。对 (2)
里的每一个被选中的 `operator->` step，只要那一步是 receiver-tied，实现就必须
把“最终经由那一步拿到的访问”视为从该步的隐式对象参数导出的。只要从整个
`E1->E2` 表达式导出的任何 borrow 或 reborrow 仍然 live，提供这些隐式对象参数
的既有绑定或者 root place，就持续受那套普通限制约束：它不能被 move-from，
不能经由该绑定被重新初始化，也不能以会让该导出访问失效的方式结束其生命周期。

(8) 只有在下列且仅在下列情况下：那个“仅仅为了完成这一个 `E1->E2` 表达式”而
执行的、最终那次隐式裸指针解引用，才会被当作安全的；因此它不需要 unsafe
context：

  (8.1) 同一个链里，每一步被选中的 `operator->` 调用全都是 receiver-tied；并且

  (8.2) 对每一个这样的被选中调用，实现都对相应 receiver 对象或 root place
  施加了 (7) 里的那套“以 receiver 为根”的 borrow 纪律。

当 (8.1)-(8.2) 满足时，实现可以依赖 wrapper type 自己的 invariant：只要相应的
receiver object 继续满足 §6.2 已经对导出 borrow / reborrow 所强制的那些状态约束，
那么每一步 `operator->` 返回出来的指针就保持有效。

(9) 如果那条链里有任何一步被选中的 `operator->` 调用不是 receiver-tied，那么
这整个 `E1->E2` 表达式就不能使用 (8) 的安全特例；(2.2) 里最终那次隐式裸指针
解引用，就受 [§5.1](01-unsafe.md#51-scppunsafe-attribute) 里的普通裸指针规则
约束。在这种情况下，整个 `E1->E2` 表达式只有在 unsafe context 里才是良构的
（well-formed）。

(10) 在按 (2) 追踪一条 `operator->` 链时，如果实现只是在内部暂时产生了某个裸
指针值，那么那个值只作为同一个 `E1->E2` 表达式里的内部瞬时 operand 存在。那
个内部指针会立刻被后续的成员访问或方法调用消费掉；它不是一个程序可以单独命名、
存储、作为实参传递、返回，或者以别的方式观察到的独立表达式值。

【注：这正是 (8) 背后的安全关键不变量：所谓“安全”情形，并不是说程序获得了
“普遍地拿到并操作裸指针”的许可，也不是说编译器会重新从零证明 wrapper 内部那
个指针天然有效；它只获得了“一次由编译器合成、且其裸指针操作数永不暴露成用户可
见值的解引用”这一个特例。同时，编译器会另外对 receiver object 施加与
`operator*` 已有 soundness 完全同一套的“导出 borrow 限制”。这类似于“带检查
的索引访问”可以在内部做指针运算，但并不会把一个未经检查的裸指针暴露给用户。——注释结束】

(11) 如果程序写了别的表达式，通过其它途径拿到一个指针——例如 `p.operator->()`
或者 `&(*p)`——那么那就是 (2) 以及 (6)-(8) 之外的事。这样的表达式完全按程序
实际写出来的那个表达式的普通规则处理，包括它可能需要满足的
`[[scpp::unsafe]]` 要求。

下面这些声明和表达式是良构的：

```cpp
struct Node {
    int value{};
};

struct OwningPtr {
    Node* ptr{};
public:
    Node* operator->() [[scpp::lifetime(self)]] { return ptr; }
    const Node* operator->() const [[scpp::lifetime(self)]] { return ptr; }
};

int read_value(OwningPtr& p) {
    return p->value;      // OK：按 (8) 安全，并且对 `p` 施加了与 `operator*` 同一套“以 receiver 为根”的 borrow 纪律
}

struct Inner {
    Node* ptr{};
public:
    Node* operator->() [[scpp::lifetime(inner)]] { return ptr; }
};

struct Outer {
    Inner inner{};
public:
    Inner& operator->() [[scpp::lifetime(outer)]] { return inner; }
};

int read_chain(Outer& o) {
    return o->value;      // OK：被选中的两步 operator-> 都是 receiver-tied，而且两个 receiver 都按 (7) 被追踪
}

struct UncheckedPtr {
    Node* ptr{};
public:
    Node* operator->() { return ptr; }
};

int read_unchecked(UncheckedPtr& p) {
    [[scpp::unsafe]] {
        return p->value;  // 只有这里才 OK：见 (9)
    }
}
```

【注：`OwningPtr` 之所以安全，并不是因为 `[[scpp::lifetime(self)]]` 神奇地证明了
它内部那个裸指针字段在全局上一定有效；它只是在与一个“返回 `Node&` 的
`operator*()` wrapper”完全同样的意义上安全：类型自己负责维持“只要 wrapper
object 本身还处在要求的状态里，它内部指针就保持有效”这个 invariant，而编译器在
(7)-(8) 里的职责，是确保只要有从 `p->...` 导出的访问还 live，`p` 就不会被
move-from、重新初始化，或者提前死亡。——注释结束】

下面这些声明或者表达式不合法（ill-formed）：

```cpp
struct BadSig {
    Node* operator->(int) { return nullptr; }
};
// 不合法：`operator->` 不能有参数

struct BadReturn {
    int operator->() { return 0; }
};
// 不合法：`operator->` 必须返回指针、class，或者到 class 的引用类型

struct LegacyBox {
    Node value{};
public:
    Node& operator*() { return value; }
    const Node& operator*() const { return value; }
};

int bad_legacy(LegacyBox& b) {
    return b->value;
}
// 不合法：class 类型不会因为定义了 `operator*` 就自动得到 `->`；必须显式定义 `operator->`

struct Proxy {};

struct BrokenChain {
    Proxy operator->() { return {}; }
};

int bad_chain(BrokenChain& p) {
    return p->value;
}
// 不合法：被选中的 `operator->` 链最终没有得到指针

struct HalfCheckedInner {
    Node* ptr{};
public:
    Node* operator->() { return ptr; }
};

struct HalfCheckedOuter {
    HalfCheckedInner inner{};
public:
    HalfCheckedInner& operator->() [[scpp::lifetime(outer)]] { return inner; }
};

int bad_safety(HalfCheckedOuter& p) {
    return p->value;
}
// 在 unsafe context 之外不合法：被选中的某一步 `operator->` 不是 receiver-tied
```

---

[← 上一节：所有权、初始化与 Move](02-ownership-and-move.md) · [目录](README.md) · [下一节：线程安全属性 →](04-thread-safety-properties.md)
