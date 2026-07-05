# 11. 模块与库（Modules & Libraries）

scpp 程序可以跨多个文件组织："一个
scpp 文件调用另一个"是有的，也有办法把一份 scpp API 分发给别的
scpp 代码使用。本章定义 scpp 程序如何跨多个文件组织，以及"库"在
scpp 里是什么。这跟 [§2.1](ch02-boundary-rules.md) 的 `extern "C"`
是两个不同的问题：那个机制是跟**真正的 C**打交道（libc、或任何其他
C 库）；本章是 scpp 代码之间互相共享。跟**既有的、原样不改的 C++
库**互操作这件事本身（任意 class、模板、重载、异常、RTTI）明确
**不追求支持**——`extern "C"` 是 scpp 跟外部世界唯一的互操作机制
（原因见 [§8](ch08-open-questions.md) 第 6 条）。

## 11.1 为什么用 C++20 modules，而不是 `#include` 头文件

scpp 原样复用真实的 C++20 module 语法——`export module name;`、
`import name;`、在具体声明上加 `export`——而不是经典的 `#include`
头文件模式，也不是另造一套新语法。理由按重要性排序有三条：

1. **健全性**。头文件本质是一份手写的、靠文本粘贴复制的前置声明。
   没有任何东西能保证它不会悄悄和另一个翻译单元里的真实定义脱节——
   而且在 scpp 里，跟普通 C++ 不一样的是，一个函数声明里**与安全
   相关的事实**（它带了哪些 `[[scpp::lifetime(name)]]`
   组）恰恰是调用者那边借用检查所依赖的东西。一份手抄的、在这一点
   上撒了谎的声明，就是一个悄无声息的健全性漏洞。这直接违背了
   [ch00](ch00-design-philosophy.md) "健全性优先于兼容性"这条原则。
   而 module 接口是从唯一真实来源编译一次、然后被**导入**的，从来
   不需要手工重打一遍——这整类 bug 因此是结构性不可能发生，而不只
   是"不鼓励"。
2. **不需要预处理器**。scpp 没有预处理器——词法层面
   完全没有 `#define`、没有宏展开、没有条件编译。C++20 modules 同样
   不需要这些东西。
3. **复用已有语法**。`export module`/`import` 是真实的、现代的、
   地道的 C++ 写法。而且很凑巧，编译器自己的实现现在就是这么写的：
   每个 `src/*.cppm` 文件本身就是一个 `export module scpp.xxx;`，
   配一个 `export namespace scpp { ... }` 作为公开面。

## 11.2 scpp 里"库"是什么（不需要新关键字）

"库"不是一个语言关键字，也不需要是：它就是若干个 `export module`
接口加上它们编译出的产物，一起打包成一个 `.scppm` 归档分发（见
[§11.11](#1111-scppm-库归档格式)）——跟真实 C++ 里"库"这个词的用法
完全一样（Boost 之所以是"一个库"，纯粹是社区约定和构建系统层面的
事，C++ 语法本身根本没定义过这个词）。包管理器、注册表、依赖解析
工具都不在本章讨论范围内（见 [§11.14](#1114-v1-不做的事backlog)）。

## 11.3 导出面与接口/实现单元的拆分

```cpp
// mylib_math.scpp —— 这个 module 的接口文件。可以直接分享，人可读。
export module mylib.math;

namespace mylib::math {
    extern int square(int x);               // 已导出，无函数体——见 §11.6
    export struct Point { int x; int y; };  // 已导出，本来就"有内容"（是数据）
}

int internal_helper(int x) { return x * 2; }  // 私有，有函数体——没导出，namespace 无所谓
```

```cpp
// mylib_math_impl.scpp —— 一个实现单元：不导出、不分享。
// 自动能看到 mylib.math 自己的接口，不需要 import。
module mylib.math;

int mylib::math::square(int x) { return x * internal_helper(x) / 2; }
```

- 一个文件只要以 `export module name;` 开头，就成为某个 module 的
  **主接口单元**（每个 module 最多一个，和真实 C++20 一样）。完全
  不写 module 声明的文件，就是一个普通的、不导出任何东西的文件，跟
  完全不涉及 module 的普通 scpp 文件一样。
- 一个以 `module name;` 开头（不带 `export`）的文件是一个**实现
  单元**：为同一个 module 贡献额外代码，自动能看到该 module 自己的
  接口，不需要 `import`。构建一个 module，就是把它的主接口单元和
  它所有的实现单元一起编译。
- 在具体声明前加 `export`（或者用 `export { ... }` 打包几个一起）
  会让它对导入方可见；文件里其余没标 `export` 的东西都是模块私有的。
- v0.1 能导出的东西，就是 v0.1 支持的子集本身能表达的东西
  （[§6](ch06-safe-subset.md)）：自由函数、
  `struct` 定义。`class`、模板等等，只要它们未来存在了，自然而然就
  能加上 `export`。每个导出的声明还必须落在一个跟 module 自己名字
  匹配的 namespace 里——见
  [§11.5](#115-导出的声明必须落在跟-module-名字匹配的-namespace-里)。
- **这修正了之前的一个范围决定**。多文件 module 原本被整个推到 v1
  范围之外；但要支持"分发编译产物而不只是分发源码"，发现至少需要
  接口单元/实现单元这个拆分，所以这个拆分现在算进 v1 了。**Module
  partition**（`export module foo:part;`，一个 module **内部**更细的
  再划分）仍然往后放——见 [§11.14](#1114-v1-不做的事backlog)。

## 11.4 Namespace

scpp 原样复用真实 C++ 的 `namespace` 语法，包括 C++17 的单行嵌套
namespace 写法（`namespace a::b::c { ... }`）。有几条刻意加的限制，
每一条都跟本文档别处已经定下的决定保持一致：

- **任何地方都不允许 `using namespace`**——哪怕只在函数内部局部用也
  不行。唯一允许的引入形式是单名 using 声明，`using foo::bar;`（这条
  规则本来就对普通名字生效，namespace 成员也一样遵守）。整体导入一个
  namespace 里所有名字，会重新引入"这个 `x` 到底是从哪来的"这种歧义，
  这正是 [ch00](ch00-design-philosophy.md) §2/§6 想要设计规避掉的东西。
- **不支持匿名 namespace**。一个 module 自己的导出面（对具体声明加
  `export`，见 [§11.3](#113-导出面与接口实现单元的拆分)）已经提供了
  "对这个编译单元私有"的效果；匿名 namespace 只会是干同一件事的第二套、
  多余的机制。
- **完全没有 ADL（参数依赖查找），永远没有**。任何不带限定符的调用，
  只从词法作用域和显式的 `using` 声明里解析，永远不看参数的类型。这是
  一条**永久**决定，不是占位——函数重载
  （[§5.10](ch05-static-checks.md)）也不需要它（它的候选集合就是普通
  名字本来就用的那套词法作用域加 `using` 声明查找），但反过来做的话，
  会重新引入"多加一个不相关的 import，悄悄改变了一个已有调用的含义"
  这类 ADL 在真实 C++ 里出了名的诡异行为，跟
  [ch00](ch00-design-philosophy.md) §2/§6 冲突。
- **namespace 别名复用真实 C++ 本来的别名语法**：`namespace cmath =
  org::lotx::cmath;`——刻意**不用** `using` 声明。`using X = Y;` 在真实
  C++ 里是**类型**别名，namespace 不是类型；照这样拼写一个 namespace
  别名，去掉 `unsafe` 之后交给真实 C++ 编译器就编不过
  （[ch00](ch00-design-philosophy.md) §6）。这是第三个、正交的机制，
  跟 `using foo::bar;`（只导入一个**名字**，不是整个 namespace）和
  `import name as local;`（[§11.7](#117-导入可见性重新导出与改名)，
  在 import 语句这一级给**module**改名，不会缩短代码里的任何路径）
  并存——三者可以自由搭配使用。
- **namespace 和 module 在其它方面仍然是正交的**，跟真实 C++ 一样：
  namespace 是纯逻辑上的名字分组；module 是物理上的编译/导入边界。
  唯一刻意做的例外，见下一节。

## 11.5 导出的声明必须落在跟 module 名字匹配的 namespace 里

```cpp
export module org.lotx.cmath;

namespace org::lotx::cmath {
    export double sqrt(double x);   // OK —— namespace 跟 module 名字匹配
}

double helper(double x) { return x; } // OK —— 没导出，namespace 无所谓
```

- **规则**：一个标了 `export` 的声明，只有词法上落在
  `namespace <M1>::<M2>::...::<Mn> { ... }` 里面才算真的导出了，这里
  `M1.M2. ... .Mn` 必须恰好是当前 module 自己的点分名字（module 名字用
  `.`，namespace 路径用 `::`——逐段互相翻译）。这是一条**前缀**要求，
  不是完全相等要求：一个 module 可以为了自己内部组织再往深处任意嵌套
  （`org::lotx::cmath::trig`、`org::lotx::cmath::stats`……），这些嵌套
  更深的声明照样能导出，因为它们的 namespace 仍然以要求的前缀开头。
  `export` 标在别的地方（namespace 不对，或者压根没有外层 namespace）
  是**编译错误**——`export` 和"落在要求的 namespace 里"是两条独立的、
  都必须满足的门槛。没标 `export` 的声明不受这条规则影响，想放哪个
  namespace（或者不放）都行。
- **刻意不引入隐式/默认 namespace**。这条规则的早期草案本来想让 module
  声明本身悄悄建立一个隐式的外层 namespace，这样接口文件就不用手写这层
  包装了。**被否决**：真实 C++ 里没有"一个没写在代码里、但仍然生效的
  namespace"这个概念——如果 scpp 自己发明一个，等把去掉 `unsafe` 后
  的文件交给真实 C++ 编译器，同一个声明会被放进**全局** namespace，跟
  scpp 自己认为这个符号该待在哪儿悄悄对不上。这正是
  [ch00](ch00-design-philosophy.md) §2/§6 要排除掉的那种隐式、无法
  erase 的魔法。显式写出来的实际代价，托 C++17 单行嵌套 namespace
  写法的福，只是接口文件里多一行——不是老版本 C++ 那种三层金字塔。
- **图什么**：这把真实 C++ 最出名的一个 include 卫生痛点
  （`std::filesystem::path`——到底该 `#include` 哪个头？——今天只能靠
  IDE 内置的启发式查找表来回答）变成了 scpp 里一个**机械保证**的事实：
  任何一个完整限定名，都能唯一确定该 `import` 哪个 module，没有例外。
  这也顺带把 [§11.9](#119-符号身份linkage-与-mangling) 里原有的
  "域名式 module 命名是**推荐约定**，编译器不强制"这条备注升级了——
  "namespace 要匹配 module 名字"这一半，现在是强制规则，不只是建议了
  （至于要不要**选**一个域名式的名字，比如 `org.lotx.cmath` 而不是裸
  的 `cmath`，仍然是约定——编译器还是没法阻止两个不相关的作者都选
  裸名字 `cmath`）。
- **Module 名字按"从外到内、反域名"的顺序读**（`org.lotx.cmath`，
  跟 Java 的 package 命名约定一致），不是正着写的 URL 风格
  （`cmath.lotx.org`）——从左往右读的顺序，必须跟对应的 namespace
  路径读起来一致（`org::lotx::cmath::sqrt`，具体的库名字在最里面/
  最后）。[§11.9](#119-符号身份linkage-与-mangling) 里的约定示例用的
  就是这个顺序。
- **跨多个 import 的 module 做限定名解析**：给定一个像
  `org::lotx::cmath::sqrt(...)` 这样的引用，解析过程沿着这个名字的
  各段，找出**恰好等于某个被 import 的 module 点分名字的最长前缀**
  （只跟当前文件**实际 import 了**的 module 比对，不会去看 search
  path 上存在的每一个 module，见
  [§11.13](#1113-import库-search-path)）；剩下的后缀，再作为一个
  namespace 路径，到那个 module 的导出面里查找。举例，如果
  `org.lotx.cmath` 和 `org.lotx` 都被 import 了，
  `org::lotx::cmath::sqrt` 会按 `org.lotx.cmath`（module）+ `sqrt`
  （它自己顶层的导出）解析——`org.lotx` 根本不会被查；只有当
  `org.lotx.cmath` 没被 import 时，才会退回去按 `org.lotx`
  （module）+ `cmath::sqrt`（**它自己**导出面里嵌套的一个
  namespace）解析。**如果两个不同的被 import 的 module 恰好都能解析
  同一个限定名**（比如 `org.lotx.cmath` 被 import 了，自己顶层导出了
  一个 `sqrt`，**同时** `org.lotx` 也被 import 了，自己又独立有一个
  嵌套的 `cmath::sqrt`）——这是一个**编译错误（"限定名有歧义"）**，
  不是静默地按最长匹配挑一个，理由跟上面否决 ADL 一样：一个不相关的、
  后来才加的 `import`，永远不该悄悄改变一个已有限定名的含义。如果
  没有任何被 import 的 module 的名字匹配任何前缀，也是编译错误，报出
  没法解析的那一段（目标是给出"是不是忘了 import X"这种质量的诊断，
  不只是"未声明的标识符"）。

## 11.6 裸 `extern`：module linkage 的无函数体声明

不带 `"C"` 字符串的 `extern`，声明一个用**普通 scpp linkage**（不是
C ABI）的函数，实现放在一个独立的实现单元里，或者放在单独分发的
编译产物里（[§11.11](#1111-scppm-库归档格式)）：

```cpp
extern int square(int x);                 // 普通 scpp linkage，跟其它函数一样受检查
extern "C" int printf(const char*, ...);  // C ABI，按 §2.1，调用永远需要 unsafe { }
```

跟 `extern "C"`（没有任何 scpp 编译器见过那个实现，
所以调用它**永远**需要 `unsafe { }`，见 §2.1）不一样，调用裸 `extern`
声明完全不需要 `unsafe { }`。信任模型不一样：module 作者把主接口单元
和它的实现单元一起构建时，编译器会**在这一次构建里**核对每个实现单元
定义的签名是否跟接口声明**完全**一致，而且那份定义会跟其它任何 scpp
函数一样受检查（见 [§5](ch05-static-checks.md)）。这跟普通 C++ 本来就
对分离编译的翻译单元抱有的"声明和定义匹配"信任是一回事（普通 C++ 里
这个信任通常只是假设、很少真被机械验证）——scpp 这里至少被一个真实的
scpp 编译器**实际检查过一次**，比普通 C++ 的保证只多不少。

## 11.7 导入可见性、重新导出与改名

- `import name;` 是**私有的、不传递的**：只让 `name` 导出的东西在
  当前文件里可见，不会转发给以后导入**这个文件**的人。
- `export import name;` 会**重新导出**，可以传递下去。
- `import name as local_name;` ——**这是新语法，真实 C++20 没有**——
  让导入方用一个本地别名称呼 `name`。这解决的是纯源码层面的问题
  （两个被导入的 module 恰好人类可读名字撞了），类似 Python 的
  `import x as y` 或者 Rust 依赖改名的效果；它**本身不解决**链接层
  的符号冲突（见 [§11.10](#1110-冲突处理)）——那是另一个独立机制。

## 11.8 健全性：跨模块检查器只需要签名

[§5.3](ch05-static-checks.md) 已经确立了 v0.1 的借用检查是**函数内
（intraprocedural）**的：检查 `f` 里对 `g` 的调用，永远只需要查
`g` 的**签名**（形参类型、返回类型、
它的 `[[scpp::lifetime(name)]]` 分组），从不
需要 `g` 的函数体。这意味着 modules 这个功能**不需要新的检查
模型**：普通同文件调用已经在用的签名查找方式，原样搬到跨模块调用上
一样成立，只是额外用从每个被导入 module 的接口里
取出的签名条目预先填充一遍——同样的查找方式、
同样的检查，只是多了一个条目来源。

struct 的内存布局也是同样道理顺带解决了：[§4.3](ch04-struct-vs-class.md)
已经把 struct 布局钉死为（字段列表、目标 triple）的纯函数——从接口
里取出的、被导入 struct 的字段列表，就是 codegen 复现一份逐字节
相同布局所需要的全部信息。

## 11.9 符号身份：linkage 与 mangling

两条不同的规则对应两种不同情况，跟一个符号到底需要不需要在自己编译
单元之外可见有关：

- **模块私有（未导出）的函数**用 LLVM `internal` linkage 生成
  （跟 C 的 `static`、C++ 的匿名 namespace 是同一套机制）。internal
  linkage 的符号永远不会被链接器拿去跨文件统一，所以两个不相关的
  module 各自都能有自己的私有 `internal_helper`，零撞车风险——**这些
  根本不需要 mangling**。
- **导出的函数**需要 external linkage（不然别的文件没法调用它），
  这意味着链接器**确实**要求这类符号全局唯一。这些会得到一个带
  module 名字的 mangled 符号：
  ```
  _scppM<长度>_<module 名字字节>F<长度>_<函数名字节>
  ```
  用长度前缀而不是分隔符，这样不管 module/函数名字取什么都不可能
  产生歧义编码。这纯粹是链接器可见的内部细节——没人会去手打它，丑
  一点没关系。
  - **超出 module 名字之外的 namespace 嵌套**：既然
    [§11.5](#115-导出的声明必须落在跟-module-名字匹配的-namespace-里)
    要求每个导出符号的 namespace 都**以**自己 module 的点分名字开头，
    上面那个 `<module 名字字节>` 段已经把这个共享前缀编码过了，不需要
    再重复编一遍。只有超出这个必需前缀之外的 namespace 嵌套（比如
    module `org.lotx.cmath` 另外把导出内容组织到
    `org::lotx::cmath::trig` 下面）才需要单独编码：每多一层嵌套，加
    一个 `N<长度>_<段字节>` block，插在 module 段和函数段之间，比如
    `org.lotx.cmath` 的 `trig::sin` 会编成
    `_scppM14_org.lotx.cmathN4_trigF3_sin`。如果一个符号直接导出在
    module 自己要求的那层 namespace（没有额外嵌套），就没有任何
    `N<长度>_` block。
  - **参数类型编码，现在定下来了**：这个位置原本是预留的，没有定编码，
    等函数重载有了设计；[§5.10](ch05-static-checks.md)
    现在给出了函数重载的设计，所以这个位置现在填上：`P<个数>_`
    后面跟着每个参数各一份、长度前缀、原样拼写的类型（比如
    `7_int32_t`、`8_int32_t&`、`14_const int32_t&`）——延续这整套方案
    一贯的长度前缀风格，不学 Itanium ABI 那种单字母缩写表。比如
    `f(int32_t, const double&)` 的参数段会编成
    `P2_7_int32_t13_const double&`。这样就够用了：因为
    [§5.10](ch05-static-checks.md) 只按类型精确匹配决议重载（不做隐式
    转换排序，见 [§6](ch06-safe-subset.md) 的"标量类型间无隐式转换"
    规则），mangled 名字只需要记录每个参数的精确拼写，不需要记录一整族
    "可能的转换目标类型"。返回类型故意**不**编码进去（C++ 自己的规则：
    不能仅凭返回类型重载）。
  - **不采用 Rust 那种 crate disambiguator hash**。Rust 在每个 mangled
    符号里多塞一个哈希，专门是为了让**同一个 crate 名字、不同版本**
    能在一次构建里安全共存——scpp v0.1 不支持这个（哪个 `.scppm`
    先在 search path 里找到就用哪个，见
    [§11.13](#1113-import库-search-path)），这个哈希要解决的问题在
    这里压根不存在。剩下的残留风险——两个不相关的库恰好选了完全
    相同的 module 名字——按 C/C++ 一直以来处理全局符号撞车的方式
    处理：硬报错，靠改名解决（见下面 [§11.10](#1110-冲突处理)），不用
    加密手段绕过去。
  - **选不选域名式命名是约定，编译器不强制**：比如
    `org.lotx.cmath`，按"从外到内、反域名"的顺序读，跟 Java 的
    package 一样（不是正着写的 URL 风格 `cmath.lotx.org`——一旦跟
    namespace 匹配挂钩，方向就不只是好看不好看的问题了，见
    [§11.5](#115-导出的声明必须落在跟-module-名字匹配的-namespace-里)）。
    这强烈推荐用于任何打算分享出去的东西——一是让意外撞名的概率低到
    几乎不可能（别人抢不走你注册的域名），二是顺带当个定位符（大家
    一看就知道去哪找 support），跟 Go 的 `github.com/user/repo`
    import path 是一个思路——跟 Java package 命名约定一样，编译器
    不要求也不检查**这部分**；不打算广泛分发的东西，起个简单的
    `cmath` 完全合法。真正**被编译器强制**的地方，是真实 C++ 和
    Java 都没有的：不管一个 module 选了什么名字，它的导出内容都必须
    落在这个名字对应的 namespace 里——见
    [§11.5](#115-导出的声明必须落在跟-module-名字匹配的-namespace-里)。
- `extern "C"` 符号**永远不**受这套方案影响——用裸的、不 mangle 的
  名字，这正是 C linkage 存在的意义。两套方案互不干扰。

## 11.10 冲突处理

- 两个名字撞了的 module 被**直接**在同一个文件里 import：在**编译期**
  就能干净地发现（既有规则：换个名字，或者用 `import ... as ...`）。
- 两个 module 通过**间接**依赖被拉进同一次最终链接（谁也没在同一个
  文件里直接 import 谁，比如两者都是某个第三方 module 的依赖，而
  那个第三方没有重新导出它们）：如果它们编译出的导出符号撞了，
  **系统链接器**在链接期报一个普通的 duplicate-symbol 错误。这跟
  普通 C/C++ 处理全局符号撞车的方式完全一样——scpp 故意不比这个更
  聪明（不采用 Rust disambiguator hash 的理由见
  [§11.9 上面](#119-符号身份linkage-与-mangling)）。

## 11.11 `.scppm` 库归档格式

一个 `.scppm` 文件是一个 **7z 归档**，可以装下一整个库——很多个
module、很多个 target-triple 变体，用户下载一次就是一个完整的
分发单元：

```
mylib.scppm  (7z 归档)
├── MANIFEST.json
├── modules/
│   ├── mylib.math.scpp          # 接口文件（§11.3），可分享的源码
│   └── mylib.collections.scpp
├── payloads/
│   ├── mylib.math/
│   │   ├── x86_64-linux-gnu.bc  # LLVM bitcode，默认/推荐的类型
│   │   ├── aarch64-linux-gnu.bc
│   │   └── x86_64-linux-gnu.o   # native object，可选的另一种类型
│   └── mylib.collections/...
└── SIGNATURE/
    ├── DIGESTS                  # 每个其他文件的 sha256，一行一个
    └── DIGESTS.asc              # 对 DIGESTS 的 OpenPGP detached 签名（可多个）
```

`MANIFEST.json` 大概长这样：

```json
{
  "format_version": 1,
  "library": { "name": "mylib", "version": "1.2.0" },
  "hash_algorithm": "sha256",
  "modules": {
    "mylib.math": {
      "interface": "modules/mylib.math.scpp",
      "dependencies": ["otherlib.util"],
      "native_link_requirements": ["m"],
      "payloads": [
        { "target_triple": "x86_64-linux-gnu", "kind": "llvm-bitcode",
          "path": "payloads/mylib.math/x86_64-linux-gnu.bc" },
        { "target_triple": "x86_64-linux-gnu", "kind": "native-object",
          "path": "payloads/mylib.math/x86_64-linux-gnu.o" }
      ]
    }
  }
}
```

- **`format_version` 最先被检查**，在解析其他任何东西之前——这样
  老工具链读到一个更新版本的归档时，会干净地报错，而不是解析到
  一半崩溃。
- **`dependencies`/`native_link_requirements`** 记录一个 module 的
  编译产物自己还依赖什么（别的 scpp module，或者 `-l` 系统库），
  这样最终链接那一步知道还要带上什么。
- **payload 的 `kind` 按条目标记，不写死成 LLVM bitcode**。
  `llvm-bitcode` 是推荐的默认类型（能在消费者自己机器上做跨模块
  LTO，也能按 target triple 分开放好几份变体——scpp 的 codegen 在
  生成代码之前就已经定死了目标的 `DataLayout`，按
  [§4.3](ch04-struct-vs-class.md)，所以产出的 bitcode 是绑定
  具体 triple 的，不是那种通用可移植的；作者想支持好几个 triple，
  就每个 triple 各放一份 `.bc`）。`native-object`/`native-archive`
  仍然是有效的备选类型，给那些不想让消费者装 LLVM 工具链的场景用。
- **接口文件里的函数体是可选的，按函数决定**。`modules/*.scpp` 里
  某个函数如果写了完整函数体，在任何 target 上都能直接从源码编译
  （完全不需要 payload）；不带函数体的（`extern`，§11.6）就要靠
  `payloads/` 里对应 target triple 的那份条目。这是按函数决定的，
  不是整个库二选一。
- **签名**：`DIGESTS` 列出归档里每个其他文件的 SHA-256；
  `DIGESTS.asc` 是对它的一个或多个 OpenPGP detached 签名——这是开源
  界用了几十年的"release tarball + `.asc`"老套路，不是自己发明一套
  方案。验证就是：重新算一遍每个列出的哈希（能查出 payload 被篡改
  /损坏），再验 PGP 签名（确认是谁在为这个包背书）。**是否强制检查
  是一个编译选项，v1 默认关闭**（比如 `--require-signed-modules`）；
  不管开不开，格式本身永远留着这个位置。**"完全没有 `SIGNATURE/`"
  和"`SIGNATURE/` 存在但验证失败"不能一视同仁**——后者是篡改或损坏
  的实锤证据，哪怕在宽松策略下也该硬报错；只有前者才是"这个库就是
  没签名"这种正常状态。
- **原子性**：一个 `.scppm` 的接口文件和 payload 永远要当成一次
  build 产出的**不可分割的一整块**——绝不能把一个归档里的
  `modules/*.scpp` 跟另一个归档里的 `payloads/` 条目混着用，哪怕
  module 同名。开了签名的话这个天然靠密码学保证（`DIGESTS` 本来就
  覆盖了每个文件）；不签名的话就是工具层面的一条不变式（没有任何
  支持的操作会去跨归档抽取/重组文件）。

## 11.12 多 target bitcode 与 target-triple 解析

一个 `.scppm` 可以打包同一个 module 的好几份 `.bc` 变体（作者想支持
哪些 target triple 就各编一份）。最终链接时，消费者的工具链在
`payloads/<module>/` 下找**精确匹配**自己 target triple 的那一份；
找不到就报错，列出这个库实际提供了哪些 triple。如果接口文件里那个
函数恰好写了完整函数体（§11.11），哪怕没有匹配的 payload，直接编译
这份内联源码也是一条可用的退路。

## 11.13 Import/库 search path

- `scpp build <file> --import name=path/to/library.scppm`——显式、无
  歧义、永远有效（对应 Clang 的 `-fmodule-file=name=path`、Rust 的
  `--extern name=path`）。
- `scpp build <file> -I <dir>`——图方便的搜索：解析 `import
  mylib.math;` 时，依次在每个 `-I` 目录下找一个**包含**字面叫
  `mylib.math` 这个 module 的 `.scppm` 归档，再在里面查
  `modules/mylib.math.scpp`。按给出的顺序，第一个找到匹配的目录
  胜出——不报歧义错误，跟 C 自己的 `-I` 头文件搜索约定一致。
  module 名字里的点，在这里同样不带目录层级含义（跟
  [§11.3](#113-导出面与接口实现单元的拆分) 一致）：编译器找的是一个
  包含那个确切 module 名字的扁平归档，不是一个嵌套路径。
- 两种方式都找不到：编译错误，报缺哪个 module。
- 最终链接时，同一个解析出来的 `.scppm` 也提供匹配消费者 target
  triple 的那份 `payloads/` 条目
  （[§11.12](#1112-多-target-bitcode-与-target-triple-解析)）。

## 11.14 v1 不做的事（backlog）

- **Module partition**（`export module foo:part;`）——往后放；v1 只有
  主接口单元/实现单元这一层拆分
  （[§11.3](#113-导出面与接口实现单元的拆分)）。
- **自动摄入 C 头文件**（`bindgen` 那种工具，或者
  `import <cheader>;`）——v1 整套 FFI 的做法就是手写
  `extern "C"`（[§2.1](ch02-boundary-rules.md)）；自动化这件事需要
  一个真正的预处理器，是架在语言之上的工具问题，不属于本章。
- **包管理/依赖解析/注册表**——完全是构建生态层面的事
  （[§11.2](#112-scpp-里库是什么不需要新关键字)），不属于语言本身。
- **可复现构建**（同一份源码编出字节相同的 `.bc`）——能让签名的信任
  链更强（独立验证一份 payload 确实对应公开源码），但 v1 不是必需的。
- **密钥吊销/信任根管理**——是 CLI/工具层的策略问题（类似 apt 自己
  维护一份独立的 trusted keyring），不属于 `.scppm` 格式本身。
- **同一个 module 名字的多版本共存**（Rust 加 disambiguator hash的
  主要理由）——不支持，见
  [§11.9](#119-符号身份linkage-与-mangling)。
- **跟既有的、原样不改的 C++ 库互操作**（真正的 class、模板、异常、
  RTTI）——明确不追求；`extern "C"` 是 scpp 提供的唯一互操作机制
  （见 [§8](ch08-open-questions.md) 第 6 条）。

---

[← 上一章：参考实现](ch10-reference-implementations.md) · [目录](README.md)
