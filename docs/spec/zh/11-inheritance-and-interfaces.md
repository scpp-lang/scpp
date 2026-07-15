# 11 继承与接口

## 11.1 总则 [class.derived]

(1) 除本条款明确修改的部分外，[class.derived]、[class.mi]、
[class.virtual]、[class.member.lookup]、[namespace.udecl]，以及 C++
关于访问控制与 derived-to-base 转换的普通规则，都原样适用于 SCPP26
程序中的继承。

(2) 当且仅当定义某个 class 的那个声明，在附着于该 class 定义的
*attribute-specifier-seq*（[dcl.attr.grammar]）里带有
attribute-token `scpp::interface` 时，这个 class 才是一个**接口**
（interface）。没有这样标记的 class 是一个**普通 class**（ordinary
class），即使它恰好没有声明任何非 static 数据成员，也仍然如此。

(3) 如果一个 class 定义的直接 base-specifier-list 中含有多于一个普通
class，那么程序不合法（ill-formed）。一个 class 除了至多一个普通直接
base class 之外，还可以额外拥有任意多个作为直接 base class 的接口。

(4) 本条款只通过 (2) 里的接口引入多重继承。它不会以其他方式放宽
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

class TagOnly {
public:
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
};  // ill-formed: two ordinary direct base classes under (3)
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

class Storage {};

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
[§11.1](11-inheritance-and-interfaces.md#111-总则-classderived) (3)，一个
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

(7) SCPP26 不要求一定用某个特定 C++ 编译器所采用的 ABI 或对象布局机制，
去实现 (6) 的保证。只要 (6) 中点名的那些可观察语义被保留下来，就已经
足够。这项许可只适用于本条款里“接口继承必须写 `virtual`”这一机制在
SCPP26 中的实现方式；它不会改变其他任何 C++ 构造所要求的可观察语义。

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

class OrdinaryBase {};

class BadVirtualOrdinary : public virtual OrdinaryBase {
public:
    ~BadVirtualOrdinary() = default;
};  // ill-formed: direct ordinary-class base uses `virtual`

class SecretMover : private virtual IMovable {
public:
    ~SecretMover() override = default;
    void move_it() override {}
    IMovable& expose_inside() { return *this; }   // OK: conversion allowed here
};

void take_movable(IMovable&);

void demo(Duck& duck, SecretMover& secret) {
    take_movable(duck);      // OK: public interface inheritance
    // take_movable(secret); // ill-formed: private base conversion denied
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
    void f(int) {}
};

class [[scpp::interface]] IDoubleOps {
public:
    void f(double) {}
};

class CombinedOps : public virtual IIntOps, public virtual IDoubleOps {
public:
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

## 11.5 多态析构与显式 override [class.dtor], [class.virtual]

(1) 就本小节而言，只要某个 class 声明了任意 virtual 成员函数，或者继承了
任意 virtual 成员函数，它就是**多态的**（polymorphic）。

(2) 一个多态 class 必须显式声明析构函数，并且这个析构函数必须是
virtual。凡是违背这条规则的完整 class 定义，都是不合法的。

(3) (2) 对接口和普通 class 一体适用；即便某个 class 只是“继承了一个
virtual 成员函数，而自己没有再声明新的 virtual 成员函数”，它也同样
适用。

(4) SCPP26 不会仅仅因为某个 class 是多态的，就去隐式合成、提升或者
重新解释一个析构函数使之成为 virtual。如果程序员没有显式声明这个
virtual 析构函数，那么程序就是不合法的。

(5) 如果某个成员函数声明或析构函数声明重写（override）了任意 base
class 的 virtual 成员函数或析构函数，那么这个声明必须带
`override` virt-specifier。凡是真正发生了 override 却省略了
`override` 的程序，都是不合法的。

(6) 如果某个声明写了 `override` virt-specifier，但它事实上并没有重写
任何 base virtual 成员函数或析构函数，那么程序就像在普通 C++ 中那样
不合法。

(7) (5) 对析构函数没有任何例外。凡是重写了 virtual base 析构函数的
派生类析构函数，都必须写成类似 `~D() override = default;` 或者
`~D() override { ... }` 这样的形式。

(8) `using`-declaration 不是一个 overriding 声明；它本身既不会满足，
也不会违反 (5)。

【注：按 (2) 必须显式写出的析构函数，是一个 user-declared destructor。
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

class MissingDtor : public Base {
public:
    void run() override {}
};  // ill-formed: polymorphic class lacks an explicit virtual destructor

class MissingOverride : public Base {
public:
    virtual ~MissingOverride() = default;   // ill-formed: overrides `Base::~Base` but omits `override`
    void run() {}                           // ill-formed: overrides `Base::run` but omits `override`
};
```

---

[← 上一节：迭代语句](10-iteration-statements.md) · [目录](README.md)
