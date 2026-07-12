# 6 所有权、初始化与 Move

## 6.1 显式初始化要求与零初始化（Required initialization and zero-initialization）[dcl.init]

(1) 一个非数组的局部变量定义，必须带有 *initializer*（[dcl.init]）。
一个非数组类型的局部变量定义，如果没有 *initializer*，不管这个变量
是什么类型、具有什么存储期，都是不合法（ill-formed）的。

【注：`int x;` 和 `Counter c;` 都是不合法的；`int x{};`、
`Counter c{};`、`Counter c{1, 2};` 和
`Counter c = make_counter();` 都是合法的。这条规则是纯语法规则：
SCPP26 不允许先写一个“无初始化的局部声明”，再靠流分析去验证它之后的赋值
是否足够。——注释结束】

(2) 一个 class 或者 struct 的非 static 数据成员，对某个特定构造函数来说，
恰好通过下列两种路径之一完成初始化：

  (2.1) 这个成员自己的声明上写了类内默认成员初始化器（in-class default
  member initializer）；或者

  (2.2) 这个构造函数的 member-initializer-list 里有一个给这个成员命名的
  member-initializer。

(3) 一个构造函数定义，可以在它的形参列表之后、function-body 之前，带一个
member-initializer-list。member-initializer-list 由 `:` 引出，并由一个或
多个 member-initializer 组成，彼此用 `,` 分隔。每个 member-initializer
都必须：

  (3.1) 命名该构造函数所属 class 或者 struct 类型的一个非 static 数据成员；
  并且

  (3.2) 用一个 *braced-init-list*（[dcl.init.list]）给这个成员提供初始值。

在 member-initializer 里使用圆括号括起来的 *expression-list* 是不合法的
（ill-formed）。

(4) 对一个 class 或者 struct 的每一个构造函数定义来说，它的每一个
非 static 数据成员，都必须通过下列两种方式之一完成初始化：

  (4.1) 这个构造函数自己的 member-initializer-list 里，有一个给该成员
  的 member-initializer；或者

  (4.2) 这个成员自己的声明上，带有类内默认成员初始化器，并且该构造函数
  的 member-initializer-list 没有给这个成员命名。

如果对某个给定构造函数来说，一个成员既不满足 (4.1) 也不满足 (4.2)，那么
这个构造函数就是不合法（ill-formed）的。一个成员在同一个
member-initializer-list 里不得被命名超过一次。

(5) 一个引用类型的非 static 数据成员，必须通过一个良定义的引用绑定来满足
(4)。因为引用没有“空状态”，所以：如果一个 class 或者 struct 带有引用成员，
那么它的某个构造函数，除非通过“能把该引用绑定到某个对象上的类内默认成员
初始化器”或者该构造函数自己的 member-initializer-list 初始化了这个成员，
否则这个构造函数就是不合法的。

(6) 一个变量定义，如果没有 *initializer*（[dcl.init]），并且不因 (1) 而
不合法，那么不管它是什么类型，都会被零初始化，而不是留下一个不确定的值：
标量对象的值是它类型要求的 `0`、`false` 或者 `0.0`；指针对象的值是空指针
值；数组类型或者 class 类型对象的每个子对象，都递归地按同一条规则被零初始化。

(7) 如果一个对象定义使用 *initializer*（[dcl.init]）来提供
direct-initialization 的实参，那么这个 *initializer* 必须是一个
*braced-init-list*（[dcl.init.list]）。在这个位置使用圆括号括起来的
*expression-list*，在 SCPP26 里不能用于初始化对象；程序不合法
（ill-formed）。

【注：`Widget x{1, 2};` 是合法的；`Widget x(1, 2);` 是不合法的。
这条规则只影响对象定义，不修改构造函数声明（例如 `Widget(int, int)`）
或者函数调用的语法。——注释结束】

【注：不过，`Widget(int x) : value{x} {}` 是按 (3) 的构造函数
member-initializer，不是按 (7) 的对象定义。——注释结束】

【注：跟 C++ 标准不一样——C++ 标准下，一个自动存储期、没有
initializer 的对象，会留下一个不确定的值（[dcl.init]），除非它的每个
子对象都是带用户提供的默认构造函数的类型——SCPP26 对这类局部声明会按
(1) 直接拒绝，对成员则按 (4) 要求“每个构造函数都把成员初始化完整”，而在
其它地方则按 (6) 要求零初始化。因此，在一份 SCPP26
程序里，不存在"读取一个值不确定的对象"这回事，也不需要任何数据流分析
去证明"每条执行路径都在使用一个局部对象之前先做了初始化"。——注释结束】

【注：(1)-(5) 不修改 union 成员或者数组声明的规则；这些仍由别的条款或者
未来的设计工作来处理。——注释结束】

```cpp
int x{};                         // OK：(1)
int y = 1;                       // OK：(1)
int z;                           // 不合法：(1)

class Defaults {
    int a{};
    int b{5};
};

class CtorOnly {
    int a;
    int b;
public:
    CtorOnly(int x, int y) : a{x}, b{y} {}
};

class Mixed {
    int a{1};
    int b;
public:
    Mixed(int x) : b{x} {}
};

int global_target{};

class RefBox {
    int& ref;
public:
    RefBox(int& r) : ref{r} {}
};

class Bad {
    int a{};
    int b;
public:
    Bad(int x) : a{x} {}   // 不合法：(4)，b 没有通过任何一条路径初始化
};
```

## 6.2 所有权与 move 状态（Ownership and move state）[basic.life]

(1) 在程序执行的任何时刻，一个自动、static、thread 或者成员存储期的
对象，都恰好处于两种状态之一：**initialized（已初始化）**或者
**moved-out（已移出）**。

(2) 一个对象在它的生命期（[basic.life]）内始终处于 initialized 状态，
除非被 (3) 或者 (4) 改变。

(3) 一个形如 `std::move(E)`的表达式，其中 `E`是指代某个对象 *obj*的
*id-expression*（[expr.prim.id]），会在这个表达式求值的那一刻，立即
把 *obj* 置于 moved-out 状态——不管这个表达式的结果之后有没有被用到、
怎么被用到。

【注：真正调用 `<utility>`里声明的函数模板 `std::move`，只是做一次
保值的、到右值引用的转换，本身对 *obj* 存储的值或者状态没有任何影响；
跟这不一样，本文档把 (3) 里这个状态转换，直接系在 `std::move(E)`这个
**语法形式**本身上，专门为了这个效果去求值——未来某个条款会列举本
文档还重新赋予了语义的其它已有语法。——注释结束】

(4) 对 *obj* 的一次赋值（[expr.assign]），或者本文档在别处定义的、
会给 *obj* 重新初始化的其它操作，会丢弃 *obj* 当前的状态和值——不管
当前是 initialized 还是 moved-out——然后把 *obj* 置于 initialized
状态，值是新赋的值。

(5) 对 *obj* 的一次**使用（use）**，是指代 *obj* 的一个 *id-expression*
的出现，但不包括：作为 (3) 里 `std::move(E)`这种表达式的操作数 `E`；
或者作为 (4) 里被重新初始化的那个对象。

(6) 一份程序，如果在执行过程中的某一点，对一个当时处于 moved-out
状态的对象做了 (5) 定义的使用，这份程序就是不合法（ill-formed）的。

【注：本条款没有给子对象（一个类成员、一个数组元素）定义独立于它
所属的完整对象自己的状态 (2)-(4)：一个子对象能不能被单独移出、而
它所属的完整对象其它部分依然保持 initialized，在什么条件下能这样，
本文档目前还没有规定。——注释结束】

## 6.3 析构（Destruction）[class.dtor]

(1) 在一个对象的存储期结束时，如果这个对象处于 initialized 状态
（6.2），它的析构函数（如果有的话）会被调用，跟 C++ 标准对这种存储期
的对象本来的要求完全一样。如果这个对象处于 moved-out 状态，不会为它
调用析构函数。

【注：本文档没有修改一个对象的存储期什么时候结束，也没有修改 C++
标准对析构施加的任何其它要求；本文档只修改了要不要调用析构函数这
一件事，依据是这个对象的所有权/move 状态（6.2）。——注释结束】

## 6.4 Move 构造与 move 赋值（Move construction and move assignment）[class.copy.ctor]、[class.copy.assign]

(1) 程序不得为一个 class 类型声明 move 构造函数（[class.copy.ctor]）
或者 move 赋值运算符（[class.copy.assign]）；一个按 C++ 标准本来的
分类会被归为其中之一的声明，是不合法（ill-formed）的。

(2) 每一个 class 类型都有一个隐式定义（implicitly-defined）的 move
构造函数，只带一个参数，类型是该 class 类型的右值引用——不管 C++
标准自己那套隐式声明的条件（[class.copy.ctor]）满不满足。

(3) 一个 class 类型有一个隐式定义的 move 赋值运算符，只带一个参数，
类型是该 class 类型的右值引用——不管 C++ 标准自己那套隐式声明的条件
（[class.copy.assign]）满不满足，除非这个 class 带有引用类型的非
static 数据成员，这种情况下它没有 move 赋值运算符——这一点和 C++
标准自己的条件（[class.copy.assign]）已经规定的完全一样。

(4) 一个 class X 隐式定义的 move 构造函数，会用构造函数参数对应的
非 static 数据成员，以适合该成员类型的方式 move 过来，按声明顺序，
初始化被构造对象的每一个非 static 数据成员。

(5) 一个 class X 隐式定义的 move 赋值运算符，会用运算符参数对应的
非 static 数据成员，以适合该成员类型的方式 move 过来，按声明顺序，
替换 `*this` 所指代的对象的每一个非 static 数据成员的值，然后返回
`*this`。

【注：如果一个非 static 数据成员本身是 class 类型，(4)、(5) 会递归
地对它适用：(2)/(3) 给这个成员自己的类型也配了一个隐式定义的 move
构造函数/move 赋值运算符，(1) 保证这不是本文档还要跟用户声明去协调
的那种声明。——注释结束】

【注：[§6.2](02-ownership-and-move.md#62-所有权与-move-状态ownership-and-move-statebasiclife)
已经规定了，一个形如 `std::move(E)` 的表达式，一旦求值，就会把它所
指代的对象置于 moved-out 状态；[§6.3](02-ownership-and-move.md#63-析构destructionclassdtor)
已经规定了，一个处于 moved-out 状态的对象会被免除析构——对于用作
初始化 (4)、(5) 参数的实参的对象，本条款不为这两个效果之一另外引入
新规则。——注释结束】

```cpp
struct Inner { int* p; };
class Outer {
    Inner a;
    int b;
public:
    Outer(int* p, int b_) : a{p}, b{b_} {}
};

Outer x{new int{1}, 2};
Outer y{std::move(x)};   // (4)：逐字段 move 构造 y.a、y.b，来自 x.a、x.b；
                          // 此后 x 处于 moved-out 状态（§6.2），如果它
                          // 声明了析构函数，也不会为它调用（§6.3）
```

## 6.5 Copy 构造与 copy 赋值（Copy construction and copy assignment）[class.copy.ctor]、[class.copy.assign]

(1) 程序可以为一个 class 类型声明 copy 构造函数（[class.copy.ctor]）
或者 copy 赋值运算符（[class.copy.assign]）。

(2) 一个 class 类型，如果没有用户声明的 copy 构造函数、没有用户
声明的析构函数、也没有用户声明的 copy 赋值运算符，就有一个隐式定义的
copy 构造函数，只带一个参数，类型是该 class 类型的 `const` 引用——不管
C++ 标准自己那套隐式声明的条件（[class.copy.ctor]）满不满足。一个
class 类型，如果有用户声明的析构函数、或者有用户声明的 copy 赋值
运算符，却没有用户声明的 copy 构造函数，就没有 copy 构造函数。

(3) 一个 class 类型，如果没有用户声明的 copy 赋值运算符、没有用户
声明的析构函数、也没有用户声明的 copy 构造函数，就有一个隐式定义的
copy 赋值运算符，只带一个参数，类型是该 class 类型的 `const` 引用——
不管 C++ 标准自己那套隐式声明的条件（[class.copy.assign]）满不满足，
除非这个 class 带有引用类型的非 static 数据成员，这种情况下它没有
copy 赋值运算符——这一点和 C++ 标准自己的条件（[class.copy.assign]）
已经规定的完全一样。一个 class 类型，如果有用户声明的析构函数、或者
有用户声明的 copy 构造函数，却没有用户声明的 copy 赋值运算符，就没有
copy 赋值运算符。

(4) 一个 class 类型有没有用户声明的 copy 构造函数，跟它有没有用户
声明的 copy 赋值运算符，是两件互不相干的事；程序可以只声明其中一个，
不声明另一个。

(5) 一个 class X 隐式定义的 copy 构造函数，会用构造函数参数对应的
非 static 数据成员，以适合该成员类型的方式 copy 过来，按声明顺序，
初始化被构造对象的每一个非 static 数据成员。

(6) 一个 class X 隐式定义的 copy 赋值运算符，会用运算符参数对应的
非 static 数据成员，以适合该成员类型的方式 copy 过来，按声明顺序，
替换 `*this` 所指代的对象的每一个非 static 数据成员的值，然后返回
`*this`。

【注：如果一个非 static 数据成员本身是 class 类型，(5)、(6) 会递归
地对它适用：按本条款，这个成员自己的类型要么有一个隐式定义的 copy
构造函数/copy 赋值运算符，要么有一个用户声明的，要么压根没有——最后
这种情况下，(5) 或者 (6)（视情况而定）对 X 就没法满足，X 也就跟着
没有隐式定义的 copy 构造函数或者 copy 赋值运算符。——注释结束】

【注：跟 [§6.4](02-ownership-and-move.md#64-move-构造与-move-赋值move-construction-and-move-assignmentclasscopyctorclasscopyassign)
不一样，本条款不禁止用户声明 copy 构造函数或者 copy 赋值运算符，而且
(5)、(6) 都完全不影响构造函数或者运算符参数所指代的那个对象——copy
跟 move 不一样，不管调用的是用户声明的还是隐式定义的，永远不会改变
被 copy 的那个对象的状态。——注释结束】

【注：(2) 里"class 类型没有隐式定义的 copy 构造函数"的那些情形，
跟 (3) 里"没有隐式定义的 copy 赋值运算符"的那些情形，正好就是 C++
标准自己那套规则里，对应特殊成员函数的隐式定义被标记为 deprecated、
而不是压根没有的那些情形（[depr.impldec]）。——注释结束】

【注：因为 (2)、(3) 在那里给出的情形下排除了 class 类型拥有隐式
定义的 copy 构造函数/copy 赋值运算符的可能性，而且 (5)、(6) 从不
修改参数所指代的对象，所以通过一个隐式定义的 copy 赋值运算符（3）
做的形如 `x = x` 的赋值，无条件是良定义的；本文档对一个用户声明的
copy 赋值运算符（1）不作此保证——这种赋值的行为完全由它自己的定义
决定，跟任何别的用户声明的函数一样。——注释结束】

```cpp
class RefCounted {
    int* count;
public:
    RefCounted(int* c) : count{c} {}
    // 用户声明：这个 class 带析构函数，所以按 (2)/(3)，本来压根不会有
    // copy 构造函数/赋值运算符
    RefCounted(const RefCounted& other) : count{other.count} { ++(*count); }
    RefCounted& operator=(const RefCounted& other) {
        if (this != &other) { count = other.count; ++(*count); }
        return *this;
    }
    ~RefCounted() { --(*count); }
};
```

## 6.6 class 类型的按值参数（By-value parameters of class type）[expr.call]

(1) 如果一个函数参数的类型是 class 类型 `T`，并且它不是引用类型，那么每次
调用时，这个参数对象都按本小节初始化。

(2) 如果对应的实参是一个 *id-expression*，指代的是某个局部对象（包括一个
参数），并且它的类型恰好就是 `T`，同时 `T` 拥有 copy 构造函数（6.5），
那么这个参数对象就从那个局部对象 copy 构造出来。

(3) 否则，对应的实参必须是一个类型为 `T` 的**新鲜值（fresh value）**。
就本文档而言，一个类型为 `T` 的新鲜值是：

  (3.1) 一个形如 `std::move(E)` 的表达式，其中 `E` 指代一个类型为 `T`
  的对象；或者

  (3.2) 一个类型为 `T` 的调用表达式。

(4) 如果既不满足 (2)，也不满足 (3)，程序就不合法（ill-formed）。

(5) 一旦按 (2) 或者 (3) 完成初始化，这个参数对象在被调用函数体内部就是一
个普通的、类型为 `T` 的自动对象，完全按
[§6.2](02-ownership-and-move.md#62-所有权与-move-状态ownership-and-move-statebasiclife)-[§6.5](02-ownership-and-move.md#65-copy-构造与-copy-赋值copy-construction-and-copy-assignmentclass.copy.ctorclass.copy.assign)
去约束，跟任何别的 class 类型局部对象没有区别。

(6) 一个候选函数，如果它那个按值 class 参数没法按本小节要求完成初始化，
那么它对重载决议来说就不是可行候选（viable）。

## 6.7 class 类型的按值返回（By-value return of class type）[stmt.return]

(1) 如果一个函数的返回类型是 class 类型 `T`，那么一条 `return` 语句的
操作数，按本小节去初始化被返回的对象。

(2) 如果这个操作数是一个 *id-expression*，指代某个局部对象（包括一个
参数），并且它的类型恰好就是 `T`，同时 `T` 拥有 copy 构造函数（6.5），
那么被返回的对象就从那个局部对象 copy 构造出来。

(3) 否则，这个操作数必须是一个类型为 `T` 的新鲜值，定义见
[§6.6](02-ownership-and-move.md#66-class-类型的按值参数by-value-parameters-of-class-typeexpr.call)
(3)。被返回的对象会从这个新鲜值 move 构造出来。

(4) 如果既不满足 (2)，也不满足 (3)，程序就不合法（ill-formed）。

(5) 一个类型为 class 类型 `T` 的调用表达式，对本小节和
[§6.6](02-ownership-and-move.md#66-class-类型的按值参数by-value-parameters-of-class-typeexpr.call)
来说，本身就是一个类型为 `T` 的新鲜值。

---

[← 上一节：`[[scpp::unsafe]]` Attribute](01-unsafe.md) · [目录](README.md) · [下一节：解引用与成员访问 →](03-dereference-and-member-access.md)
