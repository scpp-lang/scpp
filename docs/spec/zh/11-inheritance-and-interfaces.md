# 11 继承与接口

## 11.1 总则 [class.derived]

(1) 除本条款明确修改的部分外，[class.derived]、[class.mi]、
[class.virtual]、[class.member.lookup]、[namespace.udecl]，以及 C++
关于访问控制与 derived-to-base 转换的普通规则，都原样适用于 SCPP26
程序中的继承。

(2) 任何用关键字 `struct` 引入的声明，都不得：

  (2.1) 带有 *base-clause*（[class.derived]）；

  (2.2) 被 attribute-token `scpp::interface` 标记；或者

  (2.3) 声明 virtual 成员函数或 virtual 析构函数。

(3) 任何命名了某个用关键字 `struct` 声明出来的类型的 *base-specifier*，
都是不合法的（ill-formed）。

(4) (2) 与 (3) 不会对 `struct` 施加其他限制。`struct` 仍然可以像普通 C++
规则所允许的那样，声明构造函数、access-specifier、非 static 数据成员
以及非 virtual 成员函数。

(5) 当且仅当定义某个 class 的那个声明，在附着于该 class 定义的
*attribute-specifier-seq*（[dcl.attr.grammar]）里带有
attribute-token `scpp::interface` 时，这个 class 才是一个**接口**
（interface）。凡是用关键字 `class` 引入、但没有这样标记的声明，都是
一个**普通 class**（ordinary class），即使它恰好没有声明任何非 static
数据成员，也仍然如此。

(6) 如果一个 class 定义的直接 base-specifier-list 中含有多于一个普通
class，那么程序不合法（ill-formed）。一个 class 除了至多一个普通直接
base class 之外，还可以额外拥有任意多个作为直接 base class 的接口。

(7) 本条款只通过 (5) 里的接口引入多重继承。它不会以其他方式放宽
SCPP26 现有的“普通实现继承仍然是单继承”的规则。

【注：作为一种风格约定，SCPP26 源码里被 `[[scpp::interface]]` 标记的
class，推荐用前导 `I` 来命名，例如 `IReader` 或 `IMovable`。这只是一个
非规范性的建议：接口名字不遵循这个约定，本身不会让程序变成不合法。——注释结束】

```cpp
class [[scpp::interface]] IReader {
public:
    virtual ~IReader() = default;
    virtual void read() = 0;
};

struct PlainData {
private:
    int value{};
public:
    PlainData(int v) : value{v} {}
    int read() const { return value; }
};

class TagOnly {
public:
    virtual ~TagOnly() = default;
    void ping();
};

class FileReader : public virtual IReader {
public:
    ~FileReader() override = default;
    void read() override {}
};

class Bad : public FileReader, public TagOnly {
public:
    ~Bad() override = default;
};  // ill-formed: two ordinary direct base classes under (6)

struct BadStruct : public TagOnly {};  // ill-formed: a struct shall not inherit
```

## 11.2 接口声明 [dcl.attr.scpp.interface]

(1) 一个接口不得声明任何非 static 数据成员。凡是被标记为
`[[scpp::interface]]` 的 class 定义，只要声明了任意类型的非 static
数据成员，该程序就不合法（ill-formed）。

(2) (1) 并不禁止那些不会引入每对象状态的 class-scope 声明，比如类型
别名、枚举、static 数据成员、static 成员函数，或者其他不属于非 static
数据成员的声明。

(3) 一个接口的每个直接 base class 自己也都必须是接口。如果某个接口的
任意直接或传递 base class 是普通 class，那么这个接口就是不合法的。

(4) 一个接口可以声明 virtual 成员函数；这些函数既可以带函数体，也可以
带 pure-specifier。它也可以声明非 virtual 成员函数。接口里声明的非
virtual 成员函数，不属于该接口的动态派发契约；对它的调用，和普通非
virtual 成员函数完全一样。

【注：本条款没有为接口的构造函数额外引入任何特殊规则。接口可以像普通
class 一样声明构造函数，而 base-class 与 virtual-base 初始化仍然原样
遵循普通 C++ 规则。——注释结束】

(5) 如果一个程序会在任何“形成对象”的语境里形成一个完整对象，并且它的
most-derived type 就是接口，那么它就是不合法的；这类语境包括：

  (5.1) 一个按值变量定义；

  (5.2) 一个非 static 数据成员声明；

  (5.3) 一个数组元素类型；

  (5.4) 一个 `new`-expression；

  (5.5) 一个临时对象；

  (5.6) 一个按值类型的函数形参；或者

  (5.7) 一个按值返回的函数返回类型。

(6) (5) 适用时，不受“该接口是否还含有纯虚函数”的影响。即使一个接口
的所有 virtual 成员函数都有默认实现，它也仍然不能被直接实例化。

【注：(5) 防止把实现了接口的对象切片（slicing）成一个独立的接口对象。
通过引用或指针传递、返回接口，仍然是合法的，但仍受普通 C++ 关于引用
绑定、指针转换和访问控制的规则约束。一个更大的 most-derived object
内部的接口 base subobject，本身不属于 (5) 所说的完整对象。——注释结束】

【注：一个含有接口 base subobject 的 most-derived object，在 copy 或 move
时，受 [§6.4](02-ownership-and-move.md#64-move-构造与-move-赋值move-construction-and-move-assignmentclasscopyctorclasscopyassign)
与 [§6.5](02-ownership-and-move.md#65-copy-构造与-copy-赋值copy-construction-and-copy-assignmentclasscopyctorclasscopyassign)
约束；这些小节对 base-class subobject 的处理，也同样适用于这里。——注释结束】

```cpp
class [[scpp::interface]] ILogger {
    static constexpr int version = 1;
public:
    virtual ~ILogger() = default;
    virtual void log() {
        helper();
    }
    void helper() {}
};

class [[scpp::interface]] IBadState {
    int counter{};
public:
    virtual ~IBadState() = default;
};  // ill-formed: non-static data member under (1)

class Storage {
public:
    virtual ~Storage() = default;
};

class [[scpp::interface]] IBadBase : public virtual Storage {
public:
    virtual ~IBadBase() = default;
};  // ill-formed: interface inheriting an ordinary class under (3)

void consume(ILogger& ref);   // OK
void copy(ILogger value);     // ill-formed: (5.6)
ILogger make_logger();        // ill-formed: (5.7)
```

## 11.3 Base-specifier 与接口身份 [class.mi]

(1) 如果某个 class `D` 直接继承接口 `I`，那么命名 `I` 的那个
base-specifier 必须带 `virtual` 关键字。凡是没有写 `virtual` 的直接接口
base，程序都不合法。

(2) 如果某个 class `D` 直接继承普通 class `B`，那么命名 `B` 的那个
base-specifier 不得带 `virtual` 关键字。凡是给直接普通 class base 写了
`virtual` 的程序，都是不合法的。

(3) (1) 同时适用于“接口继承接口”和“普通 class 继承接口”这两种情况。

【注：(2) 在 SCPP26 中并没有拿走任何有用的表达能力。按
[§11.1](11-inheritance-and-interfaces.md#111-总则-classderived)，一个
class 至多只有一个普通直接 base class；而按
[§11.2](11-inheritance-and-interfaces.md#112-接口声明-dclattrscppinterface)
(3)，接口又只能继承别的接口。因此，普通 base 之间的关系不可能分叉成
多条路径，也不可能再从多条路径重新汇合，所以普通 C++ 里 virtual
inheritance 用来解决的那种“重复 subobject”问题，在 SCPP26 中从结构上
就不可能出现在普通 base 上。——注释结束】

(4) 一个接口 base 只能用 `public` 或 `private` 作为 access-specifier
来继承。

(5) 如果某个接口 base 以 `public` 方式继承，那么只要访问控制允许，
到该接口类型的 derived-to-base 转换，对普通外部代码以及派生类内部都
可用，但仍受程序中其他访问规则约束。如果某个接口 base 以 `private`
方式继承，那么这种转换只在该派生类自己的成员函数内可用；就本规则而
言，某个嵌套 class 的成员函数不算该派生类自己的成员函数。任意外部
代码若尝试做对应的转换，程序都不合法。

(6) 对于一个从某个 most-derived object 出发、经由一个或多个继承路径可达
的接口 base `I`，只要这些路径按 (1) 都是 virtual 的，那么它的可观察
语义必须与同样源码下普通 C++ virtual inheritance 的语义一致：把这个
most-derived object 通过任意合法路径转换到 `I`，得到的都表示同一个共享
的 `I` base 身份；并且，经由 `I` 进行 virtual dispatch 时，必须选中唯一
的 final overrider。

(7) 指向非接口类型的指针或引用，采用 *ordinary representation*
（普通表示）。它占一个 machine word，只表示被引用对象的地址。如果某个
完整的非接口 class 类型 `D` 直接或传递地实现了一个或多个接口，那么
这些接口实现都不得给 `D` 带来额外的逐对象存储；特别是，在保持 `D` 的
普通 base class 与非 static 数据成员除此之外都相同的前提下，单纯增删
接口 base 不会改变 `sizeof(D)`。

(8) 指向接口类型的指针或引用，采用 *interface representation*
（接口表示）。它恰好占两个 machine word，因此在同一目标上恰好是 (7)
所要求表示大小的两倍。其中一个 word 表示底层 most-derived object 的
地址，另一个 word 表示该接口的 dispatch 信息，足以对当前 concrete
object 上该接口所声明的每个 virtual 成员函数进行 dispatch。

(9) (8) 所述 dispatch 信息，必须在形成那个接口类型指针或引用值时就被
解析出来。此后，经由该值调用该接口所声明的 virtual 成员函数时，必须
直接使用它携带的 dispatch 信息，不得在调用点对该对象已实现的接口集合
执行搜索。

(10) 只有“指向接口的指针类型”才有 null 值。一个由 `nullptr`、zero-
initialization，或者某个指针类型成员/变量的 default-initialization 产生
出来的“指向接口的指针值”，都是一个 null interface pointer。在 null
interface pointer 里，对象地址那个 word 为零。dispatch-information
那个 word 的值是未指定的；只要对象地址那个 word 为零，程序语义就不得
依赖那个 word。

(11) 对一个“指向接口的指针值”做 nullness test——包括和 `nullptr` 比较，
以及上下文到 `bool` 的转换——只取决于 (10) 所述对象地址那个 word 是否
为零。dispatch-information 那个 word 在这种测试里不起任何作用。特别是，
两个 null interface pointer 即使携带的 dispatch-information word 不相
等，也仍然都是 null。

(12) 从接口类型指针或引用值到任何其表示只有一个 machine word 的 scalar
type，都不存在隐式或显式转换。这包括 `void*`、任何其表示只有一个
machine word 的 raw pointer type，以及像 `uintptr_t` 或 `intptr_t`
这样的整数 scalar type——只要这些类型在该目标上是一个 machine word。
凡是尝试这样转换的程序，都是不合法的。

【注：如果程序必须通过某个只接受 `void*`、`uintptr_t` 或其他单
machine-word scalar 的 API 传递一个接口值，那么它可以先把该接口值存入
一个具有稳定存储的对象里，再传递一个指向那段存储的指针，或者其他某种
应用自定义、且能保留所需信息的 handle。任何这样的指针转换，仍然受
[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) 约束。——注释结束】

(13) SCPP26 不要求必须用某个特定 C++ 编译器或任何其他实现技术所采用的
同一种 ABI、word 次序、对象布局或 dispatch-table 结构，来实现 (6)、
(8)、(9)、(10) 与 (11) 的保证。接口表示中那两个 machine word 的先后
次序未指定，(8) 所述 dispatch 信息自身的内部结构也未指定。只要这些段
落点名的可观察语义被保留即可：按 (6) 的共享接口身份、按 (7) 的单
machine-word 普通表示、按 (8) 的双 machine-word 接口表示、按 (9)
的无需调用点搜索的正确 dispatch，以及按 (10) 与 (11) 只由对象地址
word 决定的 null interface-pointer 语义。这项许可只适用于本条款中
SCPP26 对“接口继承必须写 `virtual`”及接口类型表示的实现方式；它不会
改变其他任何 C++ 构造所要求的可观察语义。

【注：因此，像 `unique_ptr<I>` 这样一个拥有所有权的指针特化——其中 `I`
是某个接口——可能需要存下接口表示的两个 word，才能在转移所有权时保留
完整的接口值。确切的库实现机制不在本条款范围内。——注释结束】

```cpp
class [[scpp::interface]] IMovable {
public:
    virtual ~IMovable() = default;
    virtual void move_it() = 0;
};

class [[scpp::interface]] IFlyable : public virtual IMovable {
public:
    ~IFlyable() override = default;
};

class [[scpp::interface]] ISwimmable : public virtual IMovable {
public:
    ~ISwimmable() override = default;
};

class Duck : public virtual IFlyable, public virtual ISwimmable {
public:
    ~Duck() override = default;
    void move_it() override {}
};

class BadDuck : public IFlyable {
public:
    ~BadDuck() override = default;
};  // ill-formed: direct interface base lacks `virtual`

class OrdinaryBase {
public:
    virtual ~OrdinaryBase() = default;
};

class BadVirtualOrdinary : public virtual OrdinaryBase {
public:
    ~BadVirtualOrdinary() override = default;
};  // ill-formed: direct ordinary-class base uses `virtual`

class SecretMover : private virtual IMovable {
public:
    ~SecretMover() override = default;
    void move_it() override {}
    IMovable& expose_inside() { return *this; }   // OK: conversion allowed here
};

void take_movable(IMovable&);

void take_userdata(void*);

struct CallbackState {
    IMovable* value;
};

void demo(Duck& duck, SecretMover& secret, CallbackState& state) {
    take_movable(duck);      // OK: public interface inheritance
    // take_movable(secret); // ill-formed: private base conversion denied

    IMovable* p = nullptr;
    if (p) {
        p->move_it();
    }

    state.value = &duck;
    // take_userdata(state.value); // ill-formed: interface pointer is not `void*`
    // auto bits = uintptr_t(state.value); // ill-formed: not a single-word scalar conversion
    take_userdata(&state);         // OK: pass pointer to stable storage instead
}
```

## 11.4 接口成员、名字查找与 virtual dispatch [class.member.lookup], [namespace.udecl], [class.virtual]

(1) 派生类中的非限定成员名查找，仍然遵循普通 C++ 规则。如果两个或更多
base class 都让同名成员变得可达，而派生类自己又没有引入能消除歧义的
声明，那么这个名字就像在 C++ 里一样是不明确（ambiguous）的。

(2) 对 (1) 里的歧义，不会因为一个候选来自普通 base class、另一个来自
接口，或者因为一个候选是 virtual、另一个不是 virtual，就被静默地自动
消解。

(3) 因而，下面每一种情况，除非程序通过普通 C++ 手段显式消解（例如：
使用限定名、在适用处声明一个新的 overriding 成员、或者写一个
`using`-declaration 把期望的 base 声明引入到派生类里），否则都是歧义：

  (3.1) 两个 sibling 接口都为同签名成员函数提供了默认实现；

  (3.2) 一个普通 base class 的成员函数，与一个接口成员函数同名；

  (3.3) 两个互不相关的 base class 声明了同名但签名不同的重载。

(4) 在 (3.3) 所述情况下，只有先把名字查找本身消解为不歧义之后，重载决议
才会开始。派生类里的 `using B::f;` 声明，会像在普通 C++ 中那样，把选中
的 base 声明引入到派生类自己的作用域里；在此之后，再对所得的重载集合
应用普通的重载决议规则。

(5) 如果两个或更多中间 base class 都重写了同一个共享 virtual base 的
同一个 virtual 函数，而某个 most-derived class 又同时继承了这些中间
base，那么这个 most-derived class 必须自己声明一个 overriding 函数，
作为唯一的 final overrider。限定名或 `using`-declaration 都不能满足
这个要求。

(6) 如果 most-derived class 里的某一个声明，确实同时重写了多个 base
virtual 函数，那么这个单独的声明可以同时满足 (5) 对这些函数的要求。

```cpp
class [[scpp::interface]] IPrintable {
public:
    virtual ~IPrintable() = default;
    virtual void print() {
        helper();
    }
    void helper() {}
};

class [[scpp::interface]] IDebuggable {
public:
    virtual ~IDebuggable() = default;
    virtual void print() {}
};

class Tool : public virtual IPrintable, public virtual IDebuggable {
public:
    ~Tool() override = default;
    // void print() override {}   // one valid explicit resolution
};

class Worker {
public:
    virtual ~Worker() = default;
    void start() {}
};

class [[scpp::interface]] IStartable {
public:
    virtual ~IStartable() = default;
    virtual void start() {}
};

class Machine : public Worker, public virtual IStartable {
public:
    ~Machine() override = default;
    // Machine m; m.start();      // ambiguous under (3.2)
};

class [[scpp::interface]] IIntOps {
public:
    virtual ~IIntOps() = default;
    void f(int) {}
};

class [[scpp::interface]] IDoubleOps {
public:
    virtual ~IDoubleOps() = default;
    void f(double) {}
};

class CombinedOps : public virtual IIntOps, public virtual IDoubleOps {
public:
    ~CombinedOps() override = default;
    using IIntOps::f;
    using IDoubleOps::f;
};

class [[scpp::interface]] ITick {
public:
    virtual ~ITick() = default;
    virtual void tick() = 0;
};

class [[scpp::interface]] ILeft : public virtual ITick {
public:
    ~ILeft() override = default;
    void tick() override {}
};

class [[scpp::interface]] IRight : public virtual ITick {
public:
    ~IRight() override = default;
    void tick() override {}
};

class Both : public virtual ILeft, public virtual IRight {
public:
    ~Both() override = default;
    void tick() override {}
};
```

## 11.5 Virtual 析构与显式 override [class.dtor], [class.virtual]

(1) 每一个 class 都必须显式声明析构函数，并且这个析构函数必须是
virtual。凡是违背这条规则的完整 class 定义，都是不合法的。

(2) (1) 一体适用于以下所有情形：该 class 不论是否声明或继承任何其他
virtual 成员函数，不论是否实现任何接口，也不论是否已经被立即拿去当作
base class 使用。

(3) SCPP26 不会隐式合成、提升或者重新解释一个析构函数使之成为
virtual。如果程序员没有显式声明这个 virtual 析构函数，那么程序就是
不合法的。

(4) 如果某个成员函数声明或析构函数声明重写（override）了任意 base
class 的 virtual 成员函数或析构函数，那么这个声明必须带
`override` virt-specifier。凡是真正发生了 override 却省略了
`override` 的程序，都是不合法的。

(5) 如果某个声明写了 `override` virt-specifier，但它事实上并没有重写
任何 base virtual 成员函数或析构函数，那么程序就像在普通 C++ 中那样
不合法。

(6) (4) 对析构函数没有任何例外。凡是重写了 virtual base 析构函数的
派生类析构函数，都必须写成类似 `~D() override = default;` 或者
`~D() override { ... }` 这样的形式。

(7) `using`-declaration 不是一个 overriding 声明；它本身既不会满足，
也不会违反 (4)。

【注：通过对每个 class 都要求满足 (1)，SCPP26 消除了“某个 class 以后
才被拿去当作 base class 使用、却缺少 virtual 析构函数”这种潜伏缺陷。
而 [§11.1](11-inheritance-and-interfaces.md#111-总则-classderived) 下的
`struct`，则是那个永远不参与继承和 virtual dispatch 的构造；它仍然可
以封装数据与行为，但不会引入隐藏的 virtual-dispatch 状态。凡是某个
class 在强制性的析构函数之外还额外声明 virtual 成员函数时，它就是在
做一个有意识的、与继承有关的设计选择。一个附带的结果是：对已经满足
(1) 的 class 来说，后续再添加 virtual 成员函数或接口 base，也不会
“新引入”这类状态。——注释结束】

【注：按 (1) 必须显式写出的析构函数，是一个 user-declared destructor。
因而，SCPP26 已经在其他地方规定给“user-declared destructor”带来的
隐式 copy 构造和 copy 赋值后果，会对接口像对其他任何 class 一样照常
生效；见 [§6.5](02-ownership-and-move.md)。本条款不会为接口引入任何
例外。——注释结束】

```cpp
class Base {
public:
    virtual void run() {}
    virtual ~Base() = default;
};

class Derived : public Base {
public:
    ~Derived() override = default;
    void run() override {}
};

class MissingDtor {
public:
    void ping() {}
};  // ill-formed: every class needs an explicit virtual destructor

class MissingOverride : public Base {
public:
    virtual ~MissingOverride() = default;   // ill-formed: overrides `Base::~Base` but omits `override`
    void run() {}                           // ill-formed: overrides `Base::run` but omits `override`
};

struct Packet {
private:
    int value{};
public:
    Packet(int v) : value{v} {}
    int read() const { return value; }
};

struct BadStructVirtual {
    virtual ~BadStructVirtual() = default;
};  // ill-formed: a struct shall not declare virtual members
```

---

[← 上一节：迭代语句](10-iteration-statements.md) · [目录](README.md)
