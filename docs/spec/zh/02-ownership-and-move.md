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
int x{};                         // OK: (1)
int y = 1;                       // OK: (1)
int z;                           // ill-formed: (1)

struct Defaults {
    int a{};
    int b{5};
};

struct CtorOnly {
    int a;
    int b;
public:
    CtorOnly(int x, int y) : a{x}, b{y} {}
};

struct Mixed {
    int a{1};
    int b;
public:
    Mixed(int x) : b{x} {}
};

int global_target{};

struct RefBox {
    int& ref;
public:
    RefBox(int& r) : ref{r} {}
};

struct Bad {
    int a{};
    int b;
public:
    Bad(int x) : a{x} {}   // ill-formed: (4), b is initialized by neither path
};
```

## 6.2 所有权、move 状态与 reborrow [basic.life]

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

(7) 如果一个引用类型的局部变量或参数、`std::span<T>` 类型的局部变量或
参数，或者 `std::span<const T>` 类型的局部变量或参数，被用来初始化另一个
引用类型或 span 类型的局部变量，或者被用来满足一个引用类型或 span 类型的
形参，并且新绑定是通过这个既有绑定去别名化同一个底层对象或范围，那么这个
新绑定就是一个 reborrow。

(8) 就 (9) 而言，如果某个按 (7) 引入的绑定，从某个程序点往前看，在某条
控制流路径上仍然可能再次被使用，那么这个 reborrow 在该程序点就是 *live*
的。一个 reborrow 的 live 范围，不要求在词法上一直延伸到它的作用域结尾；
只要后面已经不可能再发生这种使用，它就可以提前结束。

(9) 一个 mutable reborrow，只有在它所来源的那个既有绑定本身也是 mutable
时才合法。只要一个从 mutable 既有绑定形成的 reborrow 还 live：

  (9.1) 任何“通过那个既有绑定去写入”的操作，都是不合法的；并且

  (9.2) 任何“再次通过那个既有绑定形成新的 reborrow”的操作，都是不合法的。

一个“通过那个既有绑定去读取”的操作，并不会仅仅因为该 reborrow 还 live
就自动不合法；前提是这个读取本身不会修改底层对象或范围，并且也不违反本文档
的任何其它规则。等这个 reborrow 不再 live 之后，那个既有绑定才可以重新
用于写入或者再次 reborrow。

(10) 一个 shared reborrow 不会让程序比它所来源的那个绑定“更有权限”：
它不得被用来修改一个只能通过 shared 或者 `const` 绑定才能触达的对象
或者范围。

【注：(7)-(10) 里的 reborrow，要求存在一个已经存在的对象或者范围，并且新的
绑定是通过一个已经存在的引用或 span 绑定去别名化它。相反，如果一个绑定是按
(11) 去物化（materialize）一个临时对象，那么它就没有这样的 lender object。
——注释结束】

(11) 如果一个类型为 `const T&` 的局部变量或者参数，用下列任一方式完成
初始化：

  (11.1) 用一个类型为 `T` 的右值表达式，其中也包括一个类型为 `T` 的
  新鲜值（fresh value）；或者

  (11.2) 用一个表达式，并且通过选中 `T` 的某个“恰好接收这一个表达式作为
  单个参数”的构造函数，可以直接构造出一个类型为 `T` 的临时对象，

那么会物化出一个类型为 `T` 的临时对象，并让该引用绑定到这个临时对象上。

(12) 按 (11) 物化出来的临时对象，会在该引用绑定的整个生命期内保持存活。
这种绑定不是 (7)-(10) 里的 reborrow，并且对这些规则来说它不会引入 lender
object，因为它没有别名化任何预先存在的对象或范围。

【注：本条款没有给子对象（一个类成员、一个数组元素）定义独立于它
所属的完整对象自己的状态 (2)-(4)：一个子对象能不能被单独移出、而
它所属的完整对象其它部分依然保持 initialized，在什么条件下能这样，
本条款目前还没有规定。(7)-(10) 里的 reborrow 讨论的是：通过一个已经
存在的引用或 span 绑定，再形成别名；而按 (11)-(12) 去物化临时对象的绑定
并不是这种别名。无论哪一种绑定，本身都不会让完整对象进入 moved-out
状态。——注释结束】


### 跨函数生命周期分组 [dcl.attr.scpp.lifetime]

(13) attribute-token `scpp::lifetime` 可以出现在一个
*attribute-specifier-seq*（[dcl.attr.grammar]）里，并且附着于：

  (13.1) 一个参数声明，并且该参数的类型是引用类型、指针类型、
  `std::span<T>` 或者 `std::span<const T>`；或者

  (13.2) 一个这类函数或者成员函数的声明符，并且它的返回类型是引用类型、
  指针类型、`std::span<T>` 或者 `std::span<const T>`。

如果它出现在别的位置，程序就是不合法（ill-formed）的。

(14) `[[scpp::lifetime(name)]]` 恰好带一个实参。这个实参必须是一个标识符。

(15) 除 `generic` 之外，这种标识符就是一个用户写出的分组名。用户写出的分组名
只在单个函数或者成员函数声明内部有效。同样的拼写，如果出现在两个不同的声明里，
不表示任何关系；如果在同一个这类声明里重复同样的拼写，它们表示同一个
具名生命周期分组；如果拼写不同，它们表示不同的具名生命周期分组。

(16) 标识符 `generic` 是保留的。每一个带
`[[scpp::lifetime(generic)]]` 的参数，都表示一个全新的、由编译器合成的
生命周期分组；它跟下列每一项都不同：

  (16.1) 任何用户写出的分组；并且

  (16.2) 任何别的 `generic` 出现处，包括同一个声明里的另一个这种参数。

一个 `generic` 分组不会引入一个之后还可以被返回注解或者别的参数再次引用的
名字。

(17) 如果一个参数声明带有 `[[scpp::lifetime(name)]]`，并且 `name` 是用户
写出的分组名，那么这个参数就是分组 `name` 的成员。一个没有
`scpp::lifetime` attribute 的参数，不属于任何具名生命周期分组。

(18) 如果一个这类函数或者成员函数的声明符带有
`[[scpp::lifetime(name)]]`，那么它返回的那个引用、指针或者
span 值，就绑定到分组 `name`。如果出现下列任一情况，
程序不合法（ill-formed）：

  (18.1) 返回类型不是 (13.2) 里的那类可用类型；

  (18.2) `name` 是 `generic`；或者

  (18.3) 下列两者都不满足：

    (18.3.1) 这个声明里的某个显式参数属于分组 `name`；或者

    (18.3.2) 这个声明是一个 non-static 成员函数 `operator->`，并且使用了
    (23) 里的那个“隐式对象 special rule”。

(19) 一个绑定到分组 `name` 的值，只能从下列来源导出：

  (19.1) 一个或多个属于分组 `name` 的显式参数；或者

  (19.2) 对一个受 (23) 约束的 non-static 成员函数 `operator->` 来说，
  这次调用里的那个隐式对象参数；或者

  (19.3) 通过 (19.1) 或 (19.2) 那样的值可以触达的子对象、数组元素、
  基类子对象、pointee 或者连续范围。

如果一个绑定到分组 `name` 的返回值，实际上却是从下列来源导出的，那么程序
不合法（ill-formed）：

  (19.4) 另一个不同具名分组里的显式参数；

  (19.5) 一个带 `generic` 标签的参数；或者

  (19.6) 一个局部对象、临时对象，或者其它没有被证明能活过这次调用的状态。

(20) 如果多个参数属于同一个具名分组，那么这个函数可以在任何“需要该分组”
的地方，返回或者转发一个从这些参数中的任意一个导出的值。在调用点，一个绑定到
该分组的结果，会被视为至多和传给该分组全部参数里的“最短寿命那个实际实参”一样长。
不同具名分组里的参数，在生命周期上彼此独立，除非本文档的其它规则又把它们关联起来。

(21) 生命周期分组的身份只约束生命周期本身。它不会放宽 aliasing、mutability、
线程安全属性，或者 `[[scpp::unsafe]]` 的要求。尤其是：给一个裸指针加上
`[[scpp::lifetime(name)]]`，并不会允许它在 `[[scpp::unsafe]]` 之外被解引用。

(22) 用户写出的分组名是声明局部的，并且只按 α-等价比较。一个这类声明里
对另一个这类声明的调用，不会跨声明按文本拼写去比较生命周期分组名；检查器会用
被调用方自己的分组关系，去判断哪些实际实参会影响它按 (18)-(20) 返回出来的
那个可用返回值。至于一个从该分组导出的值，能不能被嵌进对象状态里，则单独由
(24) 约束。

(23) 一个非 static 成员函数，可以像自由函数一样，在它的显式参数上使用具名
生命周期分组。就本小节而言，一次对非 static 成员函数的调用，会额外提供一个
引用类型的隐式对象参数：对非 `const` 成员函数来说是 `C&`，对 `const`
成员函数来说是 `const C&`；任何经由这个隐式对象参数形成的 borrow 或 reborrow，
都受 6.2(7)-(12) 约束。这个隐式对象参数本身不能在声明里带
`[[scpp::lifetime(name)]]`。除了下面为 `operator->` 特别规定的情况之外，它也
不会单独引入一个用户写出的分组名。因此，如果一个成员函数显式写了
`[[scpp::lifetime(name)]]` 返回注解，那么除非它的某个显式参数属于分组 `name`，
否则这个程序就是不合法（ill-formed）的；一个只从 `this` 导出的值，不能满足
这个要求。不过，一个名为 `operator->` 的 non-static 成员函数，可以在它的
声明符上使用 `[[scpp::lifetime(name)]]`，把返回值直接绑定到那次调用的隐式对象
参数上，而不是绑定到某个显式参数上。在这个 special case 里，`name` 仍然是一个
“声明局部”的、用户写出的分组名；但对于那一个声明而言，在 (18.3.2) 以及
(19.2)-(19.3) 的语义里，它表示的就是那个隐式对象参数。

(24) 用一个“从具名生命周期分组导出的引用、指针或者 span”去构造对象、闭包，
或者其它会被存起来的状态，并不会抹掉这个分组原本的生命周期义务。本小节只给
(18)-(20) 里那种“函数或者成员函数直接返回出来的值”定义了生命周期分组传播；
它没有定义任何机制，让一个 class、struct、union、数组、闭包，或者其它对象
类型本身也带一个具名生命周期分组参数。因此，如果一个从具名分组或者从
`[[scpp::lifetime(generic)]]` 导出的引用、指针或者 span，会被拿去初始化或赋值给
这类对象的任意一个子对象，那么程序就是不合法（ill-formed）的。这包括：返回
`Holder{x}`（其中 `Holder` 含有一个用 `x` 初始化的引用成员）、把这样的值存进
某个数据成员或数组元素，或者把它 capture 进闭包。这个禁止项不妨碍 6.2(7)-(12)
下的普通局部 reborrow，也不妨碍把这样的值作为实参继续传给另一个调用。

(25) 生命周期分组注解可以按跟非模板完全相同的规则，出现在函数模板上，或者出现在
类模板成员上。模板实参替换既不会创建新分组，也不会合并已有分组；它只是把同一套
“声明局部的分组关系”实例化到特化后的签名上。

(26) 同一个函数或者成员函数的两个声明，在生命周期分组注解上必须一致；判断“一致”
时，允许对用户写出的分组名做一次前后一致的重命名。生命周期分组注解是函数签名里跟安全相关的事实；但重载解析不得仅仅因为生命周期分组注解不同，就把两个函数
当成可区分的不同重载。

(27) 生命周期分组注解不会修改一个类型在 §8 下的布局、triviality、thread-movable
值或者 thread-shareable 值。如果同一个声明上还带有线程安全 attribute，那么两套
要求会彼此独立、同时生效。

下面这些声明是良构的：

```cpp
const int& get_x(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return x;
}

const int& min_ref(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(a)]]
) [[scpp::lifetime(a)]] {
    return x < y ? x : y;
}

const int* pick_right(
    const int* left [[scpp::lifetime(a)]],
    const int* right [[scpp::lifetime(b)]]
) [[scpp::lifetime(b)]] {
    return right;
}

const int& keep_head(
    const int& head [[scpp::lifetime(head_life)]],
    int& scratch [[scpp::lifetime(generic)]]
) [[scpp::lifetime(head_life)]] {
    scratch = 0;
    return head;
}
```

下面这些声明是不合法（ill-formed）的：

```cpp
const int& bad_unknown(
    const int& x [[scpp::lifetime(a)]]
) [[scpp::lifetime(b)]] {
    return x;
}
// 不合法：没有任何参数引入 `b`

const int& bad_mismatch(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return y;
}
// 不合法：返回引用来自分组 `b`，不是 `a`

struct Holder {
    const int& ref;
};

Holder bad_named_store(const int& x [[scpp::lifetime(a)]]) {
    return Holder{x};
}
// 不合法：本小节没有提供让 `Holder` 携带分组 `a` 的机制

const int& bad_generic_return(
    const int& x [[scpp::lifetime(generic)]]
) [[scpp::lifetime(generic)]] {
    return x;
}
// 不合法：`generic` 是保留名，不能被返回注解点名

Holder bad_store(const int& x [[scpp::lifetime(generic)]]) {
    return Holder{x};
}
// 不合法：从 `generic` 导出的值被存进了返回状态
```

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

(4) 一个 class X 隐式定义的 move 构造函数，会按 C++ 标准对构造 X
本来要求的顺序，用构造函数参数里对应的 base-class subobject，以适合该
base 类型的方式 move 过来，完成被构造对象中每个 base-class subobject 的
move 构造。如果 X 是一个 most-derived class，且它带有 virtual base
class，那么这个 virtual base subobject 会像普通 C++ 构造那样，由 X 恰好
move 构造一次。

(5) 在 (4) 要求的 base-class subobject 之后，一个 class X 隐式定义的
move 构造函数，会用构造函数参数对应的非 static 数据成员，以适合该成员
类型的方式 move 过来，按声明顺序，初始化被构造对象的每一个非 static
数据成员。

(6) 一个 class X 隐式定义的 move 赋值运算符，会按 C++ 标准对 X
本来要求的方式，把 `*this` 所指代对象里的各个 base-class subobject，
用运算符参数里对应的 base-class subobject 完成 move 赋值。

(7) 在 (6) 要求的 base-class subobject 之后，一个 class X 隐式定义的
move 赋值运算符，会用运算符参数对应的非 static 数据成员，以适合该成员
类型的方式 move 过来，按声明顺序，替换 `*this` 所指代的对象的每一个
非 static 数据成员的值，然后返回 `*this`。

【注：如果一个 base-class subobject 或者一个非 static 数据成员本身是
class 类型，(4)-(7) 会递归地对它适用：(2)/(3) 给这个 subobject 或成员
自己的类型也配了一个隐式定义的 move 构造函数/move 赋值运算符，(1)
保证这不是本文档还要跟用户声明去协调的那种声明。——注释结束】

【注：[§6.2](02-ownership-and-move.md#62-所有权与-move-状态ownership-and-move-statebasiclife)
已经规定了，一个形如 `std::move(E)` 的表达式，一旦求值，就会把它所
指代的对象置于 moved-out 状态；[§6.3](02-ownership-and-move.md#63-析构destructionclassdtor)
已经规定了，一个处于 moved-out 状态的对象会被免除析构——对于用作
初始化 (4)、(6) 参数的实参的对象，本条款不为这两个效果之一另外引入
新规则。——注释结束】

```cpp
struct Inner { int* p; };
struct Outer {
    Inner a;
    int b;
public:
    Outer(int* p, int b_) : a{p}, b{b_} {}
};

Outer x{new int{1}, 2};
Outer y{std::move(x)};   // (5): memberwise move-constructs y.a, y.b from x.a, x.b;
                          // x is thereafter in the moved-out state (§6.2) and its
                          // destructor, if declared, is not invoked for it (§6.3)
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

(5) 一个 class X 隐式定义的 copy 构造函数，会按 C++ 标准对构造 X
本来要求的顺序，用构造函数参数里对应的 base-class subobject，以适合该
base 类型的方式 copy 过来，完成被构造对象中每个 base-class subobject 的
copy 构造。如果 X 是一个 most-derived class，且它带有 virtual base
class，那么这个 virtual base subobject 会像普通 C++ 构造那样，由 X 恰好
copy 构造一次。

(6) 在 (5) 要求的 base-class subobject 之后，一个 class X 隐式定义的
copy 构造函数，会用构造函数参数对应的非 static 数据成员，以适合该成员
类型的方式 copy 过来，按声明顺序，初始化被构造对象的每一个非 static
数据成员。

(7) 一个 class X 隐式定义的 copy 赋值运算符，会按 C++ 标准对 X
本来要求的方式，把 `*this` 所指代对象里的各个 base-class subobject，
用运算符参数里对应的 base-class subobject 完成 copy 赋值。

(8) 在 (7) 要求的 base-class subobject 之后，一个 class X 隐式定义的
copy 赋值运算符，会用运算符参数对应的非 static 数据成员，以适合该成员
类型的方式 copy 过来，按声明顺序，替换 `*this` 所指代的对象的每一个
非 static 数据成员的值，然后返回 `*this`。

【注：如果一个 base-class subobject 或者一个非 static 数据成员本身是
class 类型，(5)-(8) 会递归地对它适用：按本条款，这个 subobject 或成员
自己的类型要么有一个隐式定义的 copy 构造函数/copy 赋值运算符，要么有
一个用户声明的，要么压根没有——最后这种情况下，(5)/(6) 或者 (7)/(8)
（视情况而定）对 X 就没法满足，X 也就跟着没有隐式定义的 copy 构造函数
或者 copy 赋值运算符。——注释结束】

【注：跟 [§6.4](02-ownership-and-move.md#64-move-构造与-move-赋值move-construction-and-move-assignmentclasscopyctorclasscopyassign)
不一样，本条款不禁止用户声明 copy 构造函数或者 copy 赋值运算符，而且
(5)-(8) 都完全不影响构造函数或者运算符参数所指代的那个对象——copy
跟 move 不一样，不管调用的是用户声明的还是隐式定义的，永远不会改变
被 copy 的那个对象的状态。——注释结束】

【注：(2) 里"class 类型没有隐式定义的 copy 构造函数"的那些情形，
跟 (3) 里"没有隐式定义的 copy 赋值运算符"的那些情形，正好就是 C++
标准自己那套规则里，对应特殊成员函数的隐式定义被标记为 deprecated、
而不是压根没有的那些情形（[depr.impldec]）。——注释结束】

【注：因为 (2)、(3) 在那里给出的情形下排除了 class 类型拥有隐式
定义的 copy 构造函数/copy 赋值运算符的可能性，而且 (5)-(8) 从不
修改参数所指代的对象，所以通过一个隐式定义的 copy 赋值运算符（3）
做的形如 `x = x` 的赋值，无条件是良定义的；本文档对一个用户声明的
copy 赋值运算符（1）不作此保证——这种赋值的行为完全由它自己的定义
决定，跟任何别的用户声明的函数一样。——注释结束】

```cpp
struct RefCounted {
    int* count;
public:
    RefCounted(int* c) : count{c} {}
    // user-declared: this class has a destructor, so it would otherwise
    // have no copy constructor/assignment operator at all (2)/(3)
    RefCounted(const RefCounted& other) : count{other.count} { ++(*count); }
    RefCounted& operator=(const RefCounted& other) {
        if (this != &other) { count = other.count; ++(*count); }
        return *this;
    }
    ~RefCounted() { --(*count); }
};
```

## 6.6 新鲜值与函数形参绑定（Fresh values and function parameter binding）[expr.call]

(1) 就本文档而言，一个类型为 `T` 的**新鲜值（fresh value）**是：

  (1.1) 一个形如 `std::move(E)` 的表达式，其中 `E` 指代一个类型为 `T`
  的对象；或者

  (1.2) 一个类型为 `T` 的调用表达式；或者

  (1.3) 一个形如 `T{a1, ..., an}` 的表达式，只要它直接构造出一个类型为
  `T` 的临时对象。

(2) 一个类型为 `T` 的新鲜值，可以用于本文档任何“要求一个类型为 `T` 的
新鲜值”的位置，包括本小节以及 §6.7。

(3) 如果一个函数参数的类型是 class 类型 `T`，并且它不是引用类型，那么每次
调用时，这个参数对象都按 (4)-(7) 初始化。

(4) 如果对应的实参是一个 *id-expression*，指代的是某个局部对象（包括一个
参数），并且它的类型恰好就是 `T`，同时 `T` 拥有 copy 构造函数（6.5），
那么这个参数对象就从那个局部对象 copy 构造出来。

(5) 否则，对应的实参必须是一个类型为 `T` 的新鲜值。

(6) 如果既不满足 (4)，也不满足 (5)，程序就不合法（ill-formed）。

(7) 一旦按 (4) 或者 (5) 完成初始化，这个参数对象在被调用函数体内部就是一
个普通的、类型为 `T` 的自动对象，完全按 §6.2-§6.5 去约束，跟任何别的
class 类型局部对象没有区别。

(8) 一个候选函数，如果它那个按值 class 参数没法按 (3)-(7) 的要求完成
初始化，那么它对重载决议来说就不是可行候选（viable）。

(9) 如果一个函数参数的类型是 `const T&`，并且对应的实参满足 §6.2(11.1)
或者 §6.2(11.2)，那么这个参数会绑定到 §6.2(11) 所物化出来的那个临时
对象上，而这个临时对象的生命期由 §6.2(12) 约束。

(10) 否则，一个类型为 `const T&` 的函数参数，会按通常的引用绑定规则，直接
绑定到实参所指代的对象上。如果这个直接绑定别名化了某个已经存在的引用或
span 绑定，那么它就是一个受 §6.2(7)-(10) 约束的 reborrow。

(11) 如果既不满足 (9)，也不满足 (10)，程序就不合法（ill-formed）。

(12) 一个候选函数，如果它那个 `const T&` 参数没法按 (9)-(11) 的要求完成
初始化，那么它对重载决议来说就不是可行候选（viable）。

```cpp
struct Box {
public:
    int value;

    Box(int v) : value{v} {}
};

void consume(Box value);
int read_double(const double& x) { return x == 3.5 ? 0 : 1; }
int read_box(const Box& x) { return x.value; }
int read_text(const std::string& text) { return text.length(); }

int call_examples() {
    std::string greeting{"hello"};

    consume(Box{1});                        // OK: 6.6(1.3), 6.6(5)
    if (read_double(3.5) != 0) return 1;   // OK: 6.2(11.1), 6.6(9)
    if (read_box(Box{42}) != 42) return 2; // OK: 6.2(11.1), 6.6(9)
    if (read_text("hi") != 2) return 3;    // OK: 6.2(11.2), 6.6(9)
    if (read_text(std::move(greeting)) != 5) return 4; // OK: 6.2(11.1), 6.6(9)
    return 0;
}
```

## 6.7 class 类型的按值返回（By-value return of class type）[stmt.return]

(1) 如果一个函数的返回类型是 class 类型 `T`，那么一条 `return` 语句的
操作数，按本小节去初始化被返回的对象。

(2) 如果这个操作数恰好就是一个**没加括号的** *id-expression*，它指代的
是最内层外围函数里的某个局部对象，或者那个函数里某个**非引用**参数对象，
并且它的类型恰好就是 `T`，那么对本小节来说，这个操作数会被当作一个类型为
`T` 的新鲜值。被返回的对象会从那个对象 move 构造出来。

【注：按
[§6.4](02-ownership-and-move.md#64-move-构造与-move-赋值move-construction-and-move-assignmentclasscopyctorclasscopyassign)，
每个 class 类型总是拥有一个隐式定义的 move 构造函数。所以对满足 (2) 的
操作数，本小节总会选中 move 构造，不存在再退回去选 copy 构造函数的分支。
(2) 里的这个特殊待遇只适用于 `return` 的操作数；它不会让这种
*id-expression* 对 §6.6 来说也变成新鲜值。——注释结束】

(3) 否则，这个操作数必须是一个类型为 `T` 的新鲜值，定义见
§6.6(1)。被返回的对象会从这个新鲜值 move 构造出来。

(4) 如果既不满足 (2)，也不满足 (3)，程序就不合法（ill-formed）。

```cpp
struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(const MoveOnly&) = delete;
};

struct Box {
    int value;
};

MoveOnly make_move_only() {
    MoveOnly local{};
    return local;                 // OK: 6.7(2), move-constructs from local
}

MoveOnly pass_through(MoveOnly param) {
    return param;                 // OK: 6.7(2), move-constructs from param
}

std::string greet() {
    return std::string{"hello"};   // OK: 6.6(1.3), 6.7(3)
}

Box make_box() {
    return Box{42};                // OK: 6.6(1.3), 6.7(3)
}
```

---

[← 上一节：`[[scpp::unsafe]]` Attribute](01-unsafe.md) · [目录](README.md) · [下一节：解引用与成员访问 →](03-dereference-and-member-access.md)
