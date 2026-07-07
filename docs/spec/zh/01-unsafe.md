# 5 `[[scpp::unsafe]]` Attribute

## 5.1 Attribute（属性）[dcl.attr.scpp.unsafe]

(1) *attribute-namespace*（属性命名空间）`scpp`（[dcl.attr.grammar]）
下的 *attribute-token*（属性记号）`unsafe`，可以施加到：

  (1.1) 一个 *compound-statement* 上，通过它自己的
  *attribute-specifier-seq*（[stmt.block]）；或者

  (1.2) 一个函数上，通过一个 *function-definition*（[dcl.fct.def.general]）
  或者一个声明符指代该函数的声明（[dcl.pre]）最前面的
  *attribute-specifier-seq*。

不能带 *attribute-argument-clause*。如果一个含有 attribute-token
`unsafe` 的 *attribute-specifier-seq*，附着（appertain）在除了 (1.1)
或 (1.2) 之外的任何构造上，程序就是不合法（ill-formed）的。

【注：这里没有引入任何新语法。[stmt.block] 本来就给每一个
*compound-statement* 配了一个可选的、前置的 *attribute-specifier-seq*
（就像 C++26 的 `[[likely]] { ... }` 那样），[dcl.fct.def.general]/
[dcl.pre] 也本来就给每一个函数定义/声明配了一个可选的、最前面的
*attribute-specifier-seq*（就像 C++26 的 `[[noreturn]] void f();`
那样）；本小节只是给一个具体的 attribute-token（在本文档为自己保留的
一个 attribute-namespace 里）赋予含义，跟 [dcl.attr.fallthrough] 给
`fallthrough`、[dcl.attr.noreturn] 给 `noreturn` 赋予含义、却都不引入
任何新语法，是完全一样的做法。一个 *attribute-specifier-seq* 如果紧跟
在某个函数的 *parameters-and-qualifiers*（[dcl.fct]）后面出现——而不是
出现在该函数的 *decl-specifier-seq* 最前面，也就是 (1.2) 的位置——那么
它附着在函数的类型上，而不是附着在函数本身上，(1.1) 和 (1.2) 都不
满足。——注释结束】

(2) 如果一个程序对同一个函数声明了不止一次，并且一个含有 attribute-token
`unsafe` 的 *attribute-specifier-seq* 附着（1.2）在其中一个声明上，
那么这样的 *attribute-specifier-seq* 必须附着在这个函数的每一个声明上；
否则程序不合法（ill-formed）。

【注：这条规则排除了"在一个地方声明函数时带这个 attribute，在别的地方
却通过一个不带这个 attribute 的声明去调用它"这种写法，否则会绕开 (6)
的管制。——注释结束】

(3) 一个被含有 attribute-token `unsafe` 的 *attribute-specifier-seq*
附着的 *compound-statement*（1.1），以及一个被含有 attribute-token
`unsafe` 的 *attribute-specifier-seq* 附着的函数的整个
*function-body*（[dcl.fct.def.general]）（1.2），各自都是一个 unsafe
context（[§3.3](00-front-matter.md#3-术语和定义terms-and-definitions)）；
程序里别的任何位置都是 safe context
（[§3.2](00-front-matter.md#3-术语和定义terms-and-definitions)）。

【注：这两种情形都不是一种独立的作用域种类。本文档没有给通过 (1.1) 或
(1.2) 触达的 *compound-statement* 任何跟普通 *compound-statement*
（[stmt.block]）不一样的作用域行为：它跟任何别的 *compound-statement*
一样引入一个块作用域，块内声明的每个名字都遵循跟别的块一样的作用域
规则。——注释结束】

```cpp
int legacy_style_function(int* p, int n) {
    [[scpp::unsafe]] {
        // the whole body lives here
    }
}

[[scpp::unsafe]] int get_unchecked(int* base, int index) {
    return base[index];   // 这里不需要再嵌套一层 [[scpp::unsafe]]：
                           // 因为上面那个 attribute，(3) 已经让整个
                           // 函数体本身就是 unsafe context 了
}
```

(4) 一个按 (3) 已经是 unsafe context 的 *compound-statement* 或者
*function-body*，可以在词法上嵌套在另一个同样按 (3) 是 unsafe context
的 *compound-statement* 或者 *function-body* 里面。这种嵌套没有额外
效果：两者按 (3) 本来就都已经各自独立地是 unsafe context 了。

(5) 下列是 gated operation（[§3.4](00-front-matter.md#3-术语和定义terms-and-definitions)）：

  (5.1) 对指针类型的值做间接寻址（indirection），或者做指针算术
  （[expr.unary.op]、[expr.add]）；

  (5.2) `reinterpret_cast`（[expr.reinterpret.cast]），以及两个指针
  类型之间、且这两个类型互相都不能靠本文档允许的隐式转换互相转换时，
  做的任何 *explicit-type-conversion*（[expr.cast]）；

  (5.3) 访问一个不是 tagged union 的 union 的非 static 数据成员
  （SCPP26 的"tagged union"由未来的某个条款定义；在那样的条款加入
  本文档之前，为此处的目的，每个 union 都当作 untagged 处理）
  （[class.union]）；

  (5.4) 一个 *new-expression* 或者 *delete-expression*（[expr.new]、
  [expr.delete]）；

  (5.5) 对一个 static 或者 thread 存储期、且不是 const 限定的变量，
  做左值到右值转换（lvalue-to-rvalue conversion），或者对它赋值
  （[basic.stc.static]、[basic.stc.thread]）；

  (5.6) 调用一个 *postfix-expression* 所指代的、声明为 C 语言链接
  的函数（[dcl.link]）；

  (5.7) 调用一个 *postfix-expression* 所指代的、被一个含有
  attribute-token `unsafe` 的 *attribute-specifier-seq* 附着（1.2）的
  函数。

(6) 除非本文档明确另有声明，一个 gated operation（5），在 safe
context 里不合法（ill-formed），在 unsafe context 里合法
（well-formed）。

(7) 本文档在别的条款（不止这一条）里，对程序施加各种要求——包括但
不限于关于所有权（ownership）、别名（aliasing）、生命周期
（lifetime）、算术溢出（arithmetic overflow）的要求。除非那个条款
明确另有声明，那个条款的要求，不管它所约束的构造出现在 safe context
还是 unsafe context 里，都同样适用：本条款只放宽 (6) 里针对 (5) 列举
的这些 gated operation 的不合法性，别的一概不放宽——尤其是，不管一个
unsafe context 是通过 (3) 的哪种方式达成的，它都不会放宽任何别的条款
的要求。

【注：具体来说，如果某个未来条款要求实现对某项操作执行运行时检查
（比如对算术溢出、或者对下标越界的检查），那个条款可能会（跟 (7)
不一样）额外允许实现在 unsafe context 里跳过这项检查本身，但仍然要求
被检查的这个操作在任何上下文里都合法。跳过这样一项检查，是一项独立
于 (7) 的合法性规则、单独授予的许可（如果真的授予的话），是由引入
这项检查的那个条款授予的，不是本条款授予的。——注释结束】

## 5.2 函数指针类型（Function pointer types）[dcl.ptr.scpp.unsafe]

(1) *attribute-namespace* `scpp` 下的 attribute-token `unsafe`，也可以施加到
构成"指向函数的指针类型"的 `*` *ptr-operator*（[dcl.ptr]）上，通过这个
*ptr-operator* 自己的 *attribute-specifier-seq*。不能带
*attribute-argument-clause*。

【注：这里没有引入任何新语法：[dcl.ptr] 本来就给每一个 `*` *ptr-operator*
配了一个可选的、属于它自己的 *attribute-specifier-seq*（就像
`int* [[maybe_unused]] p;` 那样）；本小节只是给 attribute-token `unsafe`
在这个已经存在的语法位置上赋予含义，跟
[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) 在它覆盖的那两个
语法位置上赋予含义完全一样。——注释结束】

```cpp
int (* [[scpp::unsafe]] up)(int, int);   // 指向一个 unsafe-qualified
                                          // 函数类型的指针
int (*                  sp)(int, int);   // 指向一个不是 unsafe-qualified
                                    // 的函数类型的指针——按 (2)，这跟 up
                                    // 是不同的类型
```

(2) 一个被 attribute-token `unsafe` 附着 (1) 的、指向函数的指针类型（下称
*unsafe-qualified* 指向函数的指针类型），和与它别的方面都相同、但没被附着的
那个指向函数的指针类型，是两个不同的类型。

【注：这跟一个 *noexcept-specifier* 对函数类型（[dcl.fct]）的效果是同一
回事：`void(*)()` 和 `void(*)() noexcept` 同样是两个不同的类型。这两种
情形里，两个类型中的一个都对"被指向的东西能被怎样使用"多做了一个承诺，
而另一个没有，这个承诺被当成类型自身的一部分来跟踪。——注释结束】

(3) 一个由一元 `&` 运算符施加在指代某个函数的 *id-expression* 上构成的
表达式（[expr.unary.op]），或者一个指代某个函数、并被转换成指向函数的
指针类型的纯右值的 *id-expression*（[conv.func]），其类型：

  (3.1) 是 unsafe-qualified 的指向函数的指针类型，如果这个函数是一个被
  含有 attribute-token `unsafe` 的 *attribute-specifier-seq* 附着
  （[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) (1.2)）的函数，
  或者是一个声明为 C 语言链接、且没有 *function-body* 的函数
  （[dcl.link]、[dcl.fct.def.general]）；

  (3.2) 否则，是不是 unsafe-qualified 的指向函数的指针类型。

【注：(3.1) 的第二种情形，就是一个没有函数体的 `extern "C"` 声明；调用它
本来就已经是一个 gated operation（
[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) (5.6)）——理由是
一样的：取它的地址不能造出一个调用者不进 unsafe context 就能调用的指向
函数的指针类型。——注释结束】

(4) 一个不是 unsafe-qualified 的、指向函数的指针类型的纯右值，可以被转换
成与它别的方面都相同、但是 unsafe-qualified 的指向函数的指针类型的纯右值。
反过来没有这种隐式转换。

【注：这跟 [conv.fctptr] 的规则是同一回事：一个指向 `noexcept` 函数的
指针，可以转换成一个指向与它别的方面都相同、但不是 `noexcept` 的函数的
指针，反过来不行——转换只被允许指向"对拿着这个指针的代码承诺更少"的那个
类型，永远不允许指向"比造出它的那个类型承诺更多"的类型。——注释结束】

(5) 一个函数调用（[expr.call]），如果它的 *postfix-expression* 是一个
unsafe-qualified 的、指向函数的指针类型的纯右值，就是一个 gated
operation（[§3.4](00-front-matter.md#3-术语和定义terms-and-definitions)）。

【注：[§5.1](01-unsafe.md#51-attribute属性dclattrscppunsafe) (5.7) 已经
管制了一个 *postfix-expression* 按名字指代某个被 attribute-token `unsafe`
附着的函数的函数调用；那一条本身管不到"通过从这样一个函数取到的指针去
调用"这种情况，因为这时候 *postfix-expression* 指代的是一个指针值，不是
函数本身。本条款补上这个缺口。——注释结束】

```cpp
[[scpp::unsafe]] int get_unchecked(int* base, int index) { return base[index]; }
int add(int a, int b) { return a + b; }

int (* [[scpp::unsafe]] up)(int*, int) = get_unchecked;   // OK：(3.1)
int (*                  sp)(int, int)  = add;             // OK：(3.2)

int (* [[scpp::unsafe]] up2)(int, int) = add;   // OK：(4)，一次放宽方向
                                                  // 的转换
int (*                  sp2)(int*, int) = get_unchecked;  // 不合法：(4)
                                          // 不允许这个方向的转换

int r1 = up(base, 0);                       // 不合法：(5)，safe context
int r2;
[[scpp::unsafe]] { r2 = up(base, 0); }      // OK：unsafe context
int r3 = sp(1, 2);                          // OK：sp 不是 unsafe-qualified
```

---

[← 上一节：前言部分](00-front-matter.md) · [目录](README.md) · [下一节：所有权、初始化与 Move →](02-ownership-and-move.md)
