# 5. 静态检查（健全性核心）

默认情况下，编译器保证（对所支持的子集）以下性质，无条件、任何地方
都成立：

## 5.1 所有权与移动（Move / Ownership）
- 每个值有唯一所有者。
- `std::move(x)` 后，`x` 进入 *moved-out* 状态；读取 moved-out 值 → 错误。
- 重新赋值可使变量回到 *initialized* 状态。
- 作用域结束时对仍 *initialized* 的值执行 drop；对 moved-out 值不 drop。
- 具体到一个 `class` 类型的值，"move"永远是同一种编译器提供的、逐字段
  递归的操作，从来不是用户写的逻辑——完整规则见
  [§4.2](ch04-struct-vs-class.md)，为什么这么定见
  [§8](ch08-open-questions.md) Q14。

## 5.2 借用与别名（Borrow / Aliasing）
- **别名 XOR 可变**：同一对象在同一时刻，要么存在任意数量 `const T&`
  （共享借用），要么存在恰好一个 `T&`（可变借用），二者不可同时存在。
- 存在活跃借用时，不可移动或销毁被借用对象。
- 借用的来源除裸变量名外，也支持 `a.b`（字段）与 `arr[i]`（下标）。但
  v0.1 对二者都采用**整体保守**处理：借用 `a.b` 记账在 `a` 这个根上，
  借用 `a.c` 也是——二者会被判定为冲突，即便两个字段互不重叠。这与
  Rust 对动态下标数组/切片的处理一致（`arr[i]`/`arr[j]` 同样保守冲突，
  除非用 `split_at_mut` 这类显式切分 API），v0.1 只是把同样的保守规则
  也套用到了 struct 字段上，没有做 Rust 对 struct 字段那样的按字段精确
  区分。绕过办法：把每个字段分别作为**独立的函数调用实参**传递（每次
  调用的借用即借即还，先后两次调用不会重叠），或让两个具名引用各自的
  生命周期（见 §5.3 的活跃性分析）不重叠。

## 5.3 生命周期（Lifetime）
- 借用不得超过被借用者的生命周期（**禁止悬垂引用**）。
- v0.1 **只做函数内（intraprocedural）借用检查**，基于 NLL 风格的
  数据流分析（活跃性驱动的 region 推断）：一个引用局部变量的借用在其
  **最后一次使用**之后即释放，而不是等到其词法作用域结束——通过对每个
  引用局部变量做逆向（backward）活跃性分析实现，比只按词法作用域释放
  借用更精确，能接受更多合法程序（例如一个借用用完之后，即便还没到
  花括号结尾，也能立刻再借用同一个对象）。
- **不使用 `'a` 式生命周期语法**。不把生命周期做成独立的语法范畴（Rust 的
  `'a`、Circle 的 `/a`），而是用一个可选退出的 attribute
  `[[scpp::lifetime(name)]]`，附着在普通 C++ 形参/声明符语法上，给引用型
  **形参**分组。
  - **默认分组**：每个引用型输入参数，只要其类型与返回类型是引用兼容的
    （见下面的可变性授权规则），只要没被显式标记，就归入同一个隐式共享组。
    这是一种保守近似：借用检查器会把返回的引用当成**可能**别名该组内
    任意成员，因此调用点上绑定给该组的每个实参，都必须在结果被使用期间
    保持有效——哪怕函数体实际上只返回了其中一个。这是对旧规则"最多一个
    引用参数"的**严格泛化**（它接受的程序集合是旧规则的超集），且不需要
    任何标注。例如：
    ```cpp
    const std::string& longest(const std::string& x, const std::string& y) {
        return x.size() > y.size() ? x : y;
    }
    ```
    现在可以通过（`x`、`y` 一起归入默认组；调用者必须让两者都活到结果用
    完为止）——形状上和 Rust 的
    `fn longest<'a>(x: &'a str, y: &'a str) -> &'a str` 一致。
  - **用 `[[scpp::lifetime(name)]]` 退出默认组**：给某个引用参数打上这个
    attribute，会把它从默认组里**拉出来**，放进一个叫 `name` 的组。两个
    标了同一个 `name` 的参数共享一个组；不同组之间视为**互不相关**——
    检查器不假设二者有任何长短关系（谁都不保证比谁活得长），这和 Rust 里
    `'a`/`'b` 在没有 `where 'a: 'b` 时互不相关是一回事（scpp 不支持组间的
    outlives 约束；两个组如果真的需要产生关系，只能合并成一个组）。
  - **给输出的组命名**：如果函数返回引用，且参数里存在一个以上的组，输出
    借用哪个组必须用同一个 attribute 显式标在函数声明符上：
    `[[scpp::lifetime(name)]]`。只有一个组在场时（常见情形），照旧可以
    省略。一旦需要消歧，所有引用兼容的参数都必须显式打标（不能留任何一个
    隐式默认组），这样函数级的 attribute 才能明确指向一个已声明的组。
    例子（对应 Rust 的 `fn get_x<'a, 'b>(x: &'a T, y: &'b T) -> &'a T`）：
    ```cpp
    const int& get_x(const int& x [[scpp::lifetime(a)]],
                     const int& y [[scpp::lifetime(b)]]) [[scpp::lifetime(a)]] {
        return x; // 这里如果返回 y 会被拒绝：y 是组 b，声明的返回组是 a
    }
    ```
  - **按组做可变性授权**：一个组里如果没有 `T&`（可变）成员，就永远不能
    支撑 `T&` 返回，只能支撑 `const T&`——和今天单参数情形的规则一样，
    只是现在按组而不是按单个下标应用。
  - 若有 `this`/`self` 且返回引用又没有其他消歧依据，输出借用 `this`
    所在的组——`this` 作为隐式引用形参的完整处理见
    [§5.9](#59-方法与-this)。
  - 其余仍无法确定的情形 → 报错，提示添加 `[[scpp::lifetime(...)]]`
    attribute，或重构为按值/智能指针返回。
- **悬垂检查**：函数体内每条 `return` 语句，若返回类型是引用，编译器
  会沿借用链把返回表达式（裸变量、`a.b`、`arr[i]`、`*p`/`p->x`（`p`
  为 `std::unique_ptr<T>`），或对另一个"返回引用"函数的调用，递归
  展开）解析回其根，并要求这个根是归属于上面为返回类型选定的那个组
  （通过默认分组或显式 attribute 选出）的参数——否则报错拒绝。这就是
  "该函数返回的引用是否会悬垂"这个问题在 v0.1 里的具体判定方法。
- 引用可以指向 `std::unique_ptr` 指向的对象（`int& r = *p;` /
  `int& r = p->field;`，见 ch03 的 `*`/`->` 语法糖）：借用记在 `p`
  自己身上，`p` 在借用期间被 `std::move` 或重新赋值会报错（否则会
  悬垂/释放后使用）。裸指针 `T*` 的解引用仍需 `[[scpp::unsafe]]`。
- 调用一个返回引用的函数时，其结果可以直接当值使用（自动解引用，
  `int y = get_ref(x);`），也可以绑定给一个新的具名引用变量
  （`int& r = get_ref(x);`），或继续当作引用实参传给另一个函数
  （`g(get_ref(x));`）——结果会沿调用链一路解析回真正
  的根 place（可能不止一个），和裸变量借用享受同样的别名 XOR 可变检查。
  当被调函数的返回类型是从一个多成员组省略而来（上面的默认分组情形）时，
  这次调用的结果会被保守地记为**可能借用该组在这次调用点上绑定的每一个
  实参**，而不只是其中一个——只要结果还活着，对其中任何一个实参的
  失效操作（移动、写、drop）都会被拒绝。

## 5.4 初始化（Initialization）
- scpp **没有"未初始化变量"这个概念**：任何局部变量或成员，只要声明时
  没有显式初始化表达式，编译器一律保证**按位清零**（zero-init）——标量
  等价于 `0`/`false`/`0.0`，裸指针等价于 `nullptr`，`struct`/数组/
  `std::unique_ptr` 等聚合类型等价于逐字段/逐元素清零（`struct` 的具体
  规则见 [§4.1](ch04-struct-vs-class.md)）。这对所有类型一视同仁，不是
  某个类型的特例。
- 因此"禁止读取未初始化变量"是**结构性满足**的：声明的那一刻起就已经是
  一个良定义的值，不需要流敏感的数据流分析去证明"所有路径都在使用前
  初始化"。
- 这与 Rust（要求显式初始化，未初始化读取是编译错误）、普通 C++（默认不
  初始化，读取是未定义行为）都不同：scpp 选择"总是给一个良定义的默认
  值"，把"这个默认值是否是你想要的"留给程序员自己判断。

## 5.5 禁止项（除非在 `[[scpp::unsafe]]` 里）
- 裸指针解引用、指针算术。
- `reinterpret_cast`、C 风格强制转换到不兼容类型。
- `union`（未加标签的）。
- 原始 `new` / `delete`。
- 可变全局/静态变量的访问。
- 调用一个 `extern "C"` 函数。
- 调用一个自己的声明就被标记为 `[[scpp::unsafe]]` 的函数（见
  [§1.2](ch01-safety-context.md)）——这是区别于嵌套一个块的函数级
  标记，用在函数的健全性依赖某个只有调用者才能保证的前置条件的场景
  （对应 Rust 的 `unsafe fn`）。
- 通过一个自身类型就是 `[[scpp::unsafe]]`-qualified 的函数指针去调用
  （见 [§5.16](#516-函数指针function-pointers)）——跟上面一条一样的
  调用方义务，只不过延伸到了**间接**调用：这种指针背后到底是哪个函数，
  编译期不知道，可能是一个被 `[[scpp::unsafe]]` 标记的函数，也可能是一个
  没有函数体的 `extern "C"` 声明。

注意这里**没有**列上的一项：取一个裸指针的地址本身（`&expr`，见
[§5.7](#57-取地址expr与裸指针)）始终合法，跟 Rust 一样——
这里被 unsafe 拦住的是**解引用**，不是"造出"一个指针这件事本身。另外
`[[scpp::unsafe]]` 放宽的是裸指针**解引用**，不是"`const T*` 不能被写"
这条普通、无条件的类型检查规则（见
[§5.7](#57-取地址expr与裸指针)）——这条也没列在这里，
因为它本来就不是 `[[scpp::unsafe]]` 会放宽的东西。还有两个东西**确实**
跟这个列表一样在 `[[scpp::unsafe]]` 里被放宽，但理由跟上面几条不一样：
不是因为它们本来就不合法（两个从来都合法），而是因为跳过它们完全没有
"损坏的记账可能泄漏到外面代码"这种风险——这也是 §5.1-§5.4 那批检查
必须无条件继续跑的理由——`span` 的边界检查（[§8](ch08-open-questions.md)
Q1）和整数溢出检查（[§5.8](#58-整数溢出)）都是 scpp
自己加的**运行时**检查，不是本来就不合法的操作，两个都是 `[[scpp::unsafe]]`
里关、别处一律开。

`[[scpp::unsafe]]` 的具体规则见 [§1.3](ch01-safety-context.md)：它
**只**放宽这个列表，别的一概不动——本章其余检查（§5.1-§5.4）在任何
`[[scpp::unsafe]]` 上下文内依旧无条件继续跑，不管这个上下文是靠嵌套块
还是靠函数级标记建立起来的。

## 5.6 可恢复错误：`std::expected<T, E>`

scpp **没有异常**——整个语言里都没有
`throw`/`try`/`catch`（完整理由见 [§8](ch08-open-questions.md)）。所有
失败恰好分两类，是对 panic 那套已经定稿的分法（[§8](ch08-open-questions.md)
Q3）的延伸：

- **bug / 违反约定**（越界访问、前置条件不满足……）：按定义不可恢复——
  用 abort 处理，`span` 的边界检查、以及构造/析构函数（见
  [§4.2](ch04-struct-vs-class.md)）都是这个待遇。
- **可恢复的、预期内的情况**（文件不存在、输入格式错误……）：表示成一个
  普通的 `std::expected<T, E>` 值，像任何其它值一样返回，绝不抛出。

`std::expected<T, E>` 是一个**编译器内置类型**，不是真实
libstdc++/libc++ 模板的实例化，也不依赖泛型/模板先落地。跟真实
C++23 的 `std::expected` 不一样，它的访问器从不抛出——scpp 根本没有
异常机制可以让它们"抛穿"：误用（比如对一个没有值的 `expected`
解引用）是违反约定，按 scpp 里其它 bug 一样检查后 abort 处理，绝不会
抛出 `std::bad_expected_access<E>`。

**强制检查**：一次调用产生的 `std::expected<T, E>` 值不能被悄悄丢弃——
就好像每个这样的函数都隐式带了 `[[nodiscard]]`，只不过是硬性编译错误，
不是真实 C++ `[[nodiscard]]` 那种只警告的力度。完全无视一个
`std::expected` 返回值——比如把返回 `std::expected` 的函数调用当成一条
裸表达式语句、从不查看结果——在 scpp 里是**编译错误**，不是 lint。

**传播方式：现在故意就用普通的 `if`/`else`**。曾经考虑过一个类似 Rust
`?` 的后缀运算符，用来把 `std::expected` 的错误往调用者那边传播，最后
**否决**了——完整理由见 [§8](ch08-open-questions.md) Q8。简单说：跟
scpp 目前所有其它语法不一样——全都拼写成 `scpp` 命名空间下的
attribute——一个全新的运算符 token 没有"被悄悄忽略"这条退路：真正的
C++ 编译器本来就会原样接受一个它不认识的 attribute，但绝对没法越过一个
全新的运算符 token 继续解析下去，这会打破"一份合法的 scpp 文件本来就能
被真 C++ 编译器接受"这条性质（见
[ch00](ch00-design-philosophy.md) §2）。所以 v0.1 要求老老实实用普通
`if`/`else` 写传播逻辑，跟 C 用了几十年的方式一样：

```cpp
std::expected<int, ParseError> parse_and_double(const char* s) {
    std::expected<int, ParseError> r = parse_int(s);
    if (!r.has_value()) {
        return std::unexpected(r.error());
    }
    return *r * 2;
}
```

这故意是 v0.1 里传播 `std::expected` 错误的唯一方式。要不要、怎么让它
不这么啰嗦，等 C++ 标准自己在这块进一步演进之后再看——见
[§8](ch08-open-questions.md) Q8。

## 5.7 取地址（`&expr`）与裸指针

- **动机**：目前为止，本规范里一个 `T*` 值只能*被动接收*（来自一个 `extern "C"`
  形参，或者从另一个已经存在的 `T*` 复制而来），或者*靠退化产生*（定长
  数组 `T[N]` 会退化成 `T*`，见 [§3](ch03-syntactic-sugar.md)）。还是没
  有办法给一个普通的标量/struct 局部变量、一个 `.field`、或者一个
  `[index]` 元素取地址——而这恰恰是大多数真实 C API 的"输出参数"最需要
  的东西（`accept(fd, &addr, &addrlen)`、`getsockopt(fd, ..., &value,
  &len)`、`stat(path, &statbuf)`）：需要一个指向*你自己*存储的指针，
  而不是别人已经递给你的指针。本节补上这个缺口。
- **语法**：新增一个前缀一元运算符 `&expr`，`expr` 可以是
  [§5.2](#52-借用与别名borrow--aliasing) 里 `T&`/`const T&` 借用来源已经
  接受的那几种形式：普通局部变量/形参名、`.field` 投影、`[index]` 下标，
  或者 `*p`/`p->x`（`p` 是 `std::unique_ptr<T>`）——以及在上述任意一种
  基础上递归组合。如果 `expr` 解析出来的 place 只能只读访问（比如投影链
  上任何一处经过了 `const T&` 形参/绑定），`&expr` 求值得到 `const T*`；
  如果能可变访问，就得到 `T*`——这跟真实 C++ 自己的 `&expr` 规则一样，也
  跟 Rust 的 `&x as *const T` vs `&mut x as *mut T` 是同一个划分。
- **`const T*` 和 `T*` 是真正不同的两个类型**（这一节早先的草稿曾经假设
  它们是统一成一个不追踪的类型——这是错的，不管在真实 C++ 还是 scpp 里都
  不是，这一点是在讨论中被发现并纠正的，见 [§8](ch08-open-questions.md)
  Q9）。`T*` 可以隐式转换成 `const T*`（放宽权限——总是合法，跟真实 C++
  的规则一样）；`const T*` **不能**转换成 `T*`——v0.1 没有
  `const_cast`/Rust 的 `.cast_mut()` 等价物，所以现在完全没有办法从
  `const T*` 得到 `T*`。**通过 `const T*` 写，在任何上下文里都是普通的
  编译期类型错误，包括在 `[[scpp::unsafe]]` 里面**——它不在
  [§5.5](#55-禁止项除非在-scppunsafe-里) 的列表上，因为 `[[scpp::unsafe]]`
  只放宽那个列表，这条根本不属于那个列表：它跟把 `std::string` 赋值给
  `int` 是同一类普通类型不匹配，`[[scpp::unsafe]]` 显然也不会放宽后者。
  这跟 Rust 完全一致：`p: *const T` 时 `*p = x;` 哪怕在 `unsafe` 块里也会被
  拒绝。
- **造出来是 safe 的；只有拿去用才是 unsafe 的——这是 Rust 的模型，不是
  新发明的**。在真实 Rust 里，`let p = &x as *const T;` 完全是安全代码
  （`&x` 本身是一次受检查的借用；转成裸指针的强转是普通的、安全的转换）
  ——只有 `unsafe { *p }` 才需要那个逃生舱。Rust 自己的借用检查器甚至
  不会拒绝 `fn f() -> *const i32 { let x = 5; &x as *const i32 }`：裸指针
  不带生命周期参数，检查器根本没法把它和 `x` 的作用域关联起来；只有真的
  返回一个 `&i32` 引用才会被拒绝。scpp 采用完全一样的切分：`&expr`
  始终合法——**写**它不需要 `[[scpp::unsafe]]`——跟
  [§5.5](#55-禁止项除非在-scppunsafe-里) 里真正列出需要 `[[scpp::unsafe]]`
  的是裸指针**解引用**、而不是造出裸指针这件事本身，是一回事（见
  [§1.3](ch01-safety-context.md)）。得到的 `T*` 可以被存起来、传来传去、
  当返回值返回，或者干脆放着让它在指向的地方消失之后变成悬垂指针——跟
  Rust 完全一样，而且是故意如此：健全性的边界完全在后面那次 `*p`
  解引用上（已经由 `[[scpp::unsafe]]` 把关），不在 `&expr` 这一步。
- **`&expr` 求值那一刻真正会检查的东西**：跟普通读取 `expr` 一样的
  确定初始化检查（[§5.1](#51-所有权与移动move--ownership)），以及——出于
  保守考虑，因为这一刻还不知道得到的指针以后是拿去读还是写——跟新绑定一个
  `T&` 一样的排他性要求：这一刻这个根上**不能有任何已存在的借用**（不管
  共享还是可变），否则就按"再借一次 `T&`"一样的方式拒绝。但跟真正绑定一个
  `T&`/`const T&` 不一样的是，`&expr` 本身**不会**往后注册一个新的借用：
  因为产生的 `T*` 从来不被 move/borrow 跟踪（这条不变——见
  [§5.2](#52-借用与别名borrow--aliasing)），后面没有什么需要释放的，紧
  接着对同一个地方再做一次普通的 `T&`/`const T&` 借用完全不受影响。这是
  一次故意设计成"只看这一瞬间"的检查：它不能、也不打算去阻止"现在取到的
  裸指针，将来在程序某个时刻，跟同一个地方另一次单独受检查的借用发生别名
  冲突"——这跟 Rust 把这部分责任丢给 `unsafe` 代码是同一个边界。
- **和 `extern "C"` 的配合**（见 [§2.1](ch02-boundary-rules.md)）：这是
  最主要的使用动机。`T*`/`const T*` 已经是 `extern "C"` 签名接受的类型，
  `&expr` 就是普通（默认受检查）代码产出一个值去满足 C 输出参数的具体办法：
  ```cpp
  extern "C" int getsockopt(int fd, int level, int optname, void* val, int* len);
  int query(int fd) {
      int value = 0;
      int len = 4;
      [[scpp::unsafe]] {
          getsockopt(fd, 1, 2, &value, &len);
      }
      return value;
  }
  ```
  注意 `&value`/`&len` 本身不需要 `[[scpp::unsafe]]`——需要
  `[[scpp::unsafe]]` 的只是调用 `getsockopt`（一个 `extern "C"` 声明）
  这件事，这是 [§1.3](ch01-safety-context.md) 里已经有的规则（跟 `&`
  没关系）。
- **故意没包含的东西**，为了让这次新增保持一个单一、最小的目的：
  - 指针算术（`&x + 1`）——已经是 [§5.5](#55-禁止项除非在-scppunsafe-里)
    管的地盘（`[[scpp::unsafe]]` 把关），跟这次新增无关。
  - 给一个右值/临时对象取地址，或者给一个引用自己的存储取地址——`expr`
    必须能解析成一个已经存在的 place，这是它复用的借用来源语法本来的
    要求。
  - Rust 的 `&raw const`/`&raw mut`（完全不经过中间引用就取地址，Rust
    那边是给 packed struct/未初始化内存用的）——scpp 现在两个概念都没有，
    所以没有普通 `&expr` 覆盖不到、需要额外补的场景。
  - 去掉 const（`const T*` → `T*`）——v0.1 没有 `const_cast`/Rust 的
    `.cast_mut()` 等价物。如果某个真实 C API 的签名确实要非 `const`，
    而 scpp 这边的借用来源只能只读访问，v0.1 里就没法调用——已推迟（见
    [§6](ch06-safe-subset.md)）。

## 5.8 整数溢出

真实 C++ 里有符号整数溢出是**未定义行为**——哪怕 C++20 把有符号整数的
**表示方式**钉死成了补码，溢出**行为**本身依然是另一个、依然悬而未决
的问题（见 [ch00](ch00-design-philosophy.md) §8）。scpp 把这个 UB 彻底
消除，不管默认状态还是 `[[scpp::unsafe]]` 里都是，而且复用已有的"默认
受检查/`[[scpp::unsafe]]`"这条轴，不另外引入一条新的 debug/release
构建模式的轴：

- **默认状态下（`[[scpp::unsafe]]` 之外的任何地方）**：`+`、`-`、`*`
  都会检查——**有符号和无符号都查**（跟真实 C++ 不一样，那边无符号
  wrap 定义上就是"故意的"；scpp 认为无符号 wrap 跟有符号溢出一样，都很
  可能是 bug，这点跟 Rust 的判断一致）。溢出就 `abort()`，走跟 `span`
  边界检查（[§8](ch08-open-questions.md) Q1/Q3）一样的 panic 机制——
  无条件生效，不像 Rust 那样受 debug/release 编译模式影响。这里故意跟
  Rust 不一样：Rust 的检查默认只在 debug 模式生效，对真正上线、面对真实
  攻击者/真实数据的 release 二进制毫无保护；scpp 改用已有的"默认受检查/
  `[[scpp::unsafe]]`"这条轴，规范里其它地方本来就没有 debug/release
  这个维度。
- **`[[scpp::unsafe]]` 里**：不检查，但底层运算依然**不是 UB**——是保证的
  补码 wrap，跟真实 C++ 里无符号运算的行为在精神上是一回事。具体做法：
  scpp 的 codegen 从不在自己产的 `add`/`sub`/`mul` 指令上打 `nsw`/`nuw`
  （"不会有符号/无符号溢出"）标记——这两个标记正是**赋予**优化器"可以
  假设不会溢出"这张牌的东西（溢出了就是 poison value/UB）；不打这两个
  标记的普通 LLVM `add`/`sub`/`mul`，在 IR 层面本来就是良定义的、会
  wrap 的运算，没有 UB。真实 C++ 交给 Clang 编译，没法在不加整个编译
  单元级别的粗粒度 `-fwrapv` 开关的情况下拿到同样的保证（不加的话
  Clang 别无选择只能打 `nsw`，因为 C++ 标准规定这是 UB）；scpp 直接产
  自己的 IR，压根不用开这张牌。
- **为什么这一条能加入 `[[scpp::unsafe]]` 放宽的范围，而不用重新打开
  [§1.3](ch01-safety-context.md) 那条"窄的逃生舱，不是停止检查这个
  区域的开关"的规则**：跟 [§5.1-§5.4](#51-所有权与移动move--ownership)
  那批检查（move 状态、借用/别名、生命周期、zero-init）不一样——那批
  必须在 `[[scpp::unsafe]]` 里无条件继续跑，因为跳过它们会让**损坏的
  编译器记账**泄漏到块结束之后的代码里；溢出检查没有这个风险：一次不
  检查的 wrap 只是让一个普通变量拿到一个普通的（哪怕数值不对的）值——
  不会污染 move/借用/生命周期的跟踪，这些跟踪本来就跟变量具体是什么值
  无关。这个错误值真正带来的内存安全后果（比如拿去当越界下标），还是会
  被管这件事的那个检查单独拦下来（`span` 的边界检查不关心下标为什么
  错）。
- **在 `[[scpp::unsafe]]` 里手动检测溢出变得可靠了**：`if (x + 1 < x)`
  这个经典 idiom，对真实 C++ 里的有符号 `x` 是不可靠的——编译器可能（GCC/
  Clang 真的会）假设有符号溢出不会发生，把这个检查当成不可达代码优化
  掉。因为 scpp 从不打 `nsw`，没有这张牌可打，这个 idiom 就会严格按字面
  算术意思生效：`x = INT_MAX` 时 `x + 1` wrap 成 `INT_MIN`，
  `INT_MIN < INT_MAX`（正确地）为真。（这个 idiom 默认状态下没
  意义：自动检查已经在 `x + 1` 这一步就 abort 了，根本走不到能看见
  wrap 后的值的那次比较。）
- **除法/取模是单独一类，不属于"会 wrap"**：`INT_MIN / -1`（有符号除法
  自己溢出的唯一情形）以及除以 0、模以 0，都没有一个"wrap 后的结果"可以
  退回去用——硬件本身就会 trap（x86 的 `#DE`）。这两种情况无条件
  `abort()`，不管在不在 `[[scpp::unsafe]]` 里都一样——没有不检查的版本。

## 5.9 方法与 `this`

回答 [§8](ch08-open-questions.md) Q5（"`const` 成员函数怎么映射借用"）。
scpp 原样复用真实 C++ 的方法语法（trailing `const` 修饰符，不学 Rust
那种 `&self`/`&mut self` 参数写法）——新增的只是对隐式 `this` 的**借用
检查处理方式**：

- **`this` 被当成一个隐式的引用形参**，跟
  [§5.2](#52-借用与别名borrow--aliasing)/[§5.3](#53-生命周期lifetime)
  里已经覆盖的其它引用形参完全一样：`const` 方法的 `this` 按 `const T&`
  处理（对接收者的共享借用）；非 `const` 方法的 `this` 按 `T&` 处理
  （可变/独占借用）——这只是检查层面的处理；`this` 在表达式层面的实际
  拼写/类型（`this->x`、`(*this).x`）不变。
- **调用一个方法就是借用了接收者**，跟把一个引用实参传给普通函数一模
  一样：`obj.f()`，如果 `f` 是非 `const` 的，这次调用需要对 `obj` 的
  可变借用（如果 `obj` 已经被别的方式借用了，就拒绝）；`const` 方法
  只需要共享借用（可以跟别的共享借用共存，只有跟一个活跃的可变借用
  冲突时才拒绝）。这个借用按跟其它引用一样的活跃性规则释放
  （[§5.3](#53-生命周期lifetime)），不是等到整条语句结束才释放。
- **方法体内部的字段访问**（`this->field`，或者如果 scpp 的成员访问
  语法糖允许省略 `this->` 就直接写 `field`）解析回根 `this`，跟今天
  `a.field` 解析回根 `a`完全一样——包括现有的"整体保守"处理
  （[§5.2](#52-借用与别名borrow--aliasing)）：`this->field1` 和
  `this->field2` 记账在同一个根上，会冲突，跟普通 struct 类型局部变量
  `a` 的 `a.field1`/`a.field2` 今天的待遇一样。这里不需要新规则——把
  `this` 当成普通引用处理，这个结论就自动成立。
- **`const` 会传导到字段访问**，跟真实 C++ 一致：`const` 方法内部，
  `this->field = x` 会被拒绝（通过一个按 `const T&` 处理的 `this` 去
  写），通过 `this` 调用某个类类型字段上的非 `const` 方法也一样被
  拒绝——这两个都只是"不能通过共享借用去改"这条普通规则
  （[§5.2](#52-借用与别名borrow--aliasing)）套用到 `this` 上，不是
  新增的检查。
- **`mutable` 字段是上面这条规则唯一的、故意留的例外**（完整设计见
  [§4.2](ch04-struct-vs-class.md)）：通过 `this` 读写一个 `mutable`
  字段，不管 `this` 是不是按 `const` 处理都允许——但对 `mutable` 字段
  取引用或者取地址，无条件拒绝（不只是在 `const` 方法里才拒绝），这正
  是让这条例外保持健全、又不需要任何运行时检查的关键：一个永远不可能
  被引用的值，不管查不查，都不可能产生别名。
- **[§5.3](#53-生命周期lifetime) 里那条 `this` 省略规则现在真正生效
  了**：之前定了但一直"睡着"（"v0.1 尚无 class 方法/`this` 概念，这条
  规则暂不适用"）——一个返回引用、又没有其它生命周期分组消歧依据的
  方法，现在真的默认借用 `this` 所在的组。这跟 Rust 自己的第三条生命
  周期省略规则是一回事（有 `&self`/`&mut self` 时，省略的输出生命周期
  默认绑定到它的生命周期）。
- **调用任何方法都要求 `this` 指向的对象处于已初始化状态**（不是
  moved-out）——跟任何其它解引用一样的确定初始化前提条件
  （[§5.4](#54-初始化initialization)），没有方法特有的东西。
- **通过 `this` 把某个字段 move 出去**（比如一个 `unique_ptr` 字段）
  受跟普通引用一样的规则限制：只拿着一个借用（非 `const` 方法永远只有
  `T&`，从来没有真正拥有接收者本身），不能把 `*this` 或者它的某个字段
  move 出去。Rust 撞到的是同一堵墙，用 `std::mem::take`/`Option::take()`
  （"换一个合法的占位值进去，同时把旧值 move 出来"）绕过去——scpp 现在
  还没设计对应的写法；这里先标记成一个具体的开放后续问题，不是这一节
  顺带解决的。

## 5.10 函数重载

回答 [§8](ch08-open-questions.md) Q11。scpp 允许多个函数（自由函数或
方法）共用一个名字，只按参数列表区分——**永远不**靠返回类型区分（这条
规则 [ch11](ch11-modules-and-libraries.md) 的 mangling 方案从一开始就
留好了位置）。真实 C++ 自己的重载决议是靠给隐式转换序列排等级（精确
匹配 > 提升 > 转换 > ……），实际用起来比看着复杂得多：提升只会精确指向
`int`/`unsigned int`/`double`，不是"离得最近的更宽类型"，所以哪个重载
会赢，取决于哪个内置类型恰好是这台机器的 `int`——而两个都只是"普通
转换"级别的候选（比如 `int16_t` 和 `int64_t` 竞争一个 `int32_t` 实参）
在真实 C++ 里直接判歧义，压根没有"更近的赢"这条规则（设计这一节的时候
拿真实编译器验证过，不是凭空假设）。

- **决议规则：只按类型精确匹配。** [§6](ch06-safe-subset.md) 已经确立
  scpp 标量类型之间没有任何隐式转换（把 `bool`/`char` 原来的规则推广到
  整个数值家族）——所以重载决议根本不需要转换等级算法：一个候选可行，
  当且仅当它每个参数的类型都跟对应实参的类型完全一样。因为两个重载不
  可能声明成完全相同的参数类型列表（那是普通的重复定义错误，不是两个
  重载），**类型精确匹配这一步本身永远不会产生歧义结果**——最终结果
  只有"恰好找到一个匹配"或者"一个都没匹配上"（后者报编译错误，要求在
  调用处显式转换，跟其它类型不匹配的情况一样）。这是故意跟真实 C++
  不一样的地方，改学 Rust/Swift/Kotlin（见 [§8](ch08-open-questions.md)
  Q11）。
- **按值 vs 按引用是另一条、正交的轴。** `f(T)`/`f(T&)`/`f(const T&)`
  算三个不同的、合法的重载——这在 scpp 里特别有用，因为它们分别意味着
  转移所有权/可变借用/共享借用。这里不需要新的消歧义逻辑：
  [§5.1](#51-所有权与移动move--ownership) 已经要求显式写
  `std::move(x)` 才能把一个具名位置的值移出去，所以一个裸左值实参只能
  匹配 `T&`/`const T&` 参数，永远不会匹配 `T`；反过来 `std::move(x)`
  （或者普通的纯右值/临时对象）只会匹配 `T` 参数。当一个可变（非
  `const`）左值同时能匹配 `T&` 和 `const T&` 时，`T&` 赢——直接照抄真实
  C++ 自己的消歧义规则（这正是让 `T& get(); const T& get() const;` 能
  当成两个合法重载工作起来的原因，正好填上
  [§5.9](#59-方法与-this) 里标记过的那个坑）。
- **作用域规则照抄普通 C++ 名字查找。** 内层作用域声明的同名函数，会
  把外层**整个**重载集合隐藏掉（不会跨作用域合并）——这是任何其它声明
  本来就有的规则，不是给重载专门加的新东西。一条
  `using foo::bar;`声明（[ch11](ch11-modules-and-libraries.md)）会导入
  `foo::bar` 处可见的**所有**重载，不只是一个。
- **跟 ADL 没有任何关系。** 一次不带限定符的调用，候选集合就是词法
  作用域加显式 `using` 声明查找本来就会收集到的那一批
  （[ch11](ch11-modules-and-libraries.md)）——跟 scpp 完全没有参数依赖
  查找这条决定一致，而且因为没有 ADL 反而更简单。
- **歧义总体上仍然是硬编译错误**——只是对 v0.1 这种纯标量类型的重载
  集合来说，类型精确匹配这条规则碰巧让歧义变得不可能出现；这条规则是
  作为一般性原则声明的（不只是"现在恰好不会发生"），为将来某个新特性
  （首当其冲是模板/泛型）重新引入歧义可能性的那天做准备。
- **这一轮明确不做的**：涉及模板/泛型函数的重载决议（跟模板一起往后
  放）、默认参数值（一个独立的、还没设计的特性）。把一个重载名字取地址
  当函数指针用，之前说等函数指针本身有了设计再定，现在
  [§5.16](#516-函数指针function-pointers) 给出了规则：用目标指向函数的
  指针类型（来自正在初始化的声明、正在传的参数、或者一次显式转换）去
  挑出参数列表和返回类型都精确匹配的那一个重载——直接复用真实 C++ 自己
  对 `&overloaded_name` 的目标类型规则，而且因为类型精确匹配（上面）
  保证了对给定目标类型最多只有一个候选能匹配，这条规则依然是确定性的。

## 5.11 泛型函数与 Concept

回答 [§8](ch08-open-questions.md) Q12。scpp 对编译期多态的答案，原样
复用真实 C++20 的 `concept`/`requires` 语法，以及简写的函数模板形式
（`Concept auto` 参数）——刻意用它代替继承/虚函数（那两个仍然搁置，见
[§4.2](ch04-struct-vs-class.md)）。每次调用都单态化（按每个具体类型
各生成一份代码，跟真实 C++ 模板/Rust 泛型一样），所以是零开销：没有
vtable，压根不走运行时分发。

```cpp
template<typename T>
concept Shape = requires(const T& t) {
    { t.area() } -> std::same_as<double>;
};

void print_area(const Shape auto& s) {
    // s.area() 合法——concept 保证了这一点；关于 s 的其它任何东西都不保证
}
```

- **`concept` 是对一个类型的编译期谓词，写法和检查方式跟真实 C++20
  完全一样**——`concept`/`requires` 本身的语法和语义都没有改动。
- **满足关系是结构化的，跟真实 C++ 一样（不是名义化的，不学 Rust 的
  `impl Trait for Type`）。** 一个类型满足一个 concept，纯粹靠它自己
  有没有匹配的成员——不存在、也不要求任何"这个类型实现了这个
  concept"的显式声明。这是经过权衡的、刻意的选择：Rust 的名义化模型
  （显式 `impl` 块）能避开一类真实存在的 bug（两个毫不相关的
  concept，恰好都要求一个同名同形的方法，一个类型可能"意外"同时满足
  两者，被稀里糊涂地匹配上）——但真实 C++ 没有 `impl` 块这种语法可以
  复用，发明一个就正好是
  [ch00](ch00-design-philosophy.md) §2/§6 要反对的那种新的、无法
  erase 的语法。**只有以后真实 C++ 标准自己加了名义化的 opt-in
  机制才重新考虑**——跟否决 `??` 运算符时用的是同一条解决方式（见
  [§8](ch08-open-questions.md) Q8）。
- **一个 concept 约束的函数，函数体在它自己定义的地方就完整检查一遍，
  把约束参数的类型当成抽象的来处理**——函数体里只能用 concept 的
  `requires` 表达式**确实**保证过的操作，别的一律在这个泛型函数自己
  的定义处报编译错误，不管将来拿哪个具体类型实例化都一样。这是刻意
  跟真实 C++ 模板（哪怕加了 concept 约束）不一样的地方——真实 C++
  模板大部分函数体类型检查还是延迟到实例化的时候才做（"两阶段查找"），
  这也是为什么真实 C++ 模板的报错信息，出了名地跟触发实例化的那个具体
  类型绑在一起，而不是指向泛型定义本身。scpp 这里改学 Rust 的
  trait-bound 模型，而且这也是必须的——不然没法保持本章检查一贯的
  "函数内（intraprocedural）"性质（见
  [§11.6](ch11-modules-and-libraries.md)）——泛型函数没有一个单一的、
  具体的签名可以拿来检查函数体，除非这么做。
- **约束参数的 concept 是可选的，不是必须的。** 裸写 `auto`（前面不带
  任何 concept 名字）是合法的——意味着这个参数的类型被当成完全不透明
  的：函数体可以 move 它、存它、传给另一个接受兼容（裸写或者约束更松）
  参数的函数、把它 return 出去，但对它调用任何方法、用任何运算符，都是
  编译错误——就跟它绑了一个 `requires` 表达式什么都不保证的 concept
  一样。绑一个具名的 concept（`Concept auto`），就能解锁这个 concept
  的 `requires` 表达式额外保证的东西，检查方式还是同一套"定义处一次性
  检查"。这条规则对泛型类型自己的类型参数同样适用（见
  [§5.14](#514-泛型类型)）。
- **参数包在两种泛型函数写法里都支持，但函数体里能做的事情不完全一样。**
  简写形式的参数包（`Concept auto&... args`）支持，但只能通过 fold
  expression 使用——真实 C++17 语法原样复用（`(pack op ...)`、
  `(... op pack)`，或者带初值）。fold expression 的含义对**任意长度**
  的 pack（包括空 pack）都是良定义的，所以检查它只需要检查"折叠用的
  那个操作符，对 concept 保证的东西成不成立"这一件事——跟检查任何别的
  concept 保证过的操作没有本质区别，而且照样是在泛型函数自己的定义处
  一次性检查完：
  ```cpp
  template<typename T>
  concept Formattable = requires(const T& t, std::ostream& os) { os << t; };

  void log(const Formattable auto&... args) {
      (std::cout << ... << args) << "\n";
  }
  ```
  **完整 header 形式**的参数包
  （`template<typename... Args> void f(Args... args)`）现在也支持，而且
  因为模板 header 里直接给这个 pack 起了名字，它还可以被继续转发到另一个
  调用或构造的实参列表里（`g(args...)`、`new T(args...)`）。**仍然不支持
  的，是在函数体内部递归拆包**（剥出第一个元素、递归处理剩下的，经典
  C++ 变参模板写法）——每一层递归都需要单独检查一份签名，跟"定义处
  一次性检查"这条原则冲突。递归拆包是泛型**类型**自己存储结构的构建
  方式（见 [§5.14](#514-泛型类型)）——那边是类型定义本身在做递归，
  每一层依然是各自一次性检查完，不是函数体的控制流。
- **compound requirement（`{ expr } -> Constraint;`）约束返回值必须是
  精确类型，写成 `std::same_as<T>`**——永远不用
  `std::convertible_to<T>`，也永远不能是裸类型名（`-> T` 单独写压根
  不是合法 C++ 语法：那个位置按语法规定必须是个 concept 名字，不能是
  类型名，这条设计的时候拿真实编译器验证过）。`std::convertible_to`
  在 scpp 里反正也没意义，因为 [§6](ch06-safe-subset.md) 已经确立 scpp
  标量类型之间没有隐式转换——"可转换"和"完全一样"在这里是同一回事。
  这样约束出来的精确类型，就是泛型函数体里可以把这个表达式的结果当成
  的类型。
- **simple requirement（`{ expr };`，没有 `->` 子句）对返回值类型什么
  都不约束**——跟上一条一致，泛型函数体只能把这种表达式当成一条被
  丢弃结果的语句来用（为了它的副作用调用一下）；把它的结果绑定给
  任何东西，或者以任何依赖类型的方式使用它，都是编译错误，因为在
  "定义时一次性检查"这个前提下，根本没有类型可以拿来推理。
- **type-requirement（`typename T::Foo;`）和嵌套 requirement（任意的
  编译期布尔常量表达式）v0.1 不支持**——前者是因为 scpp 现在还没有
  关联类型/嵌套类型别名这个机制，后者是因为它是个远比这一轮范围更
  开放式的特性（对类型的任意编译期谓词）。
- **泛型函数既可以用 C++20 的简写形式**（`void f(Concept auto& x)`）
  **也可以用完整 header 形式**（`template<Concept T> void f(T& x)`）——
  真实 C++ 把这两种写法当成完全等价，scpp 这里也一样，没有偏好、也
  没有限制哪一种。完整 header 形式还能多支持两件事：多个类型参数
  （`template<typename T, typename U> void f(T& a, U& b)`，跟泛型
  类型一致，见 [§5.14](#514-泛型类型)），以及"只在返回类型里出现"的
  泛型函数——类型参数完全不绑在任何函数参数的声明位置上，调用点必须
  显式指定（比如 `template<typename T> T make(); make<Circle>();`）——
  这是简写形式表达不出来的，因为简写形式天生要求受约束的参数绑在某个
  函数参数自己的声明位置上。这正是 [§5.14](#514-泛型类型) 的"从基类
  推导"访问函数模式（`get<I>`）需要的：调用点显式指定、不靠推导的
  非类型实参，以及一个命名着某个类模板特化的参数类型——两者都不是
  `Concept auto`占位符能写出来的。
- **concept 里不能带默认方法体**——跟 Rust 的 trait 不一样，Rust trait
  可以提供一个默认实现，类型可以继承或者覆盖它；真实 C++ 的
  `concept` 纯粹是个结构化谓词，压根不能带函数体。这不是 scpp 专门加的
  限制，真实 C++ 里 `concept` 本来就是这个意思。
- **mangling 不需要新机制**。一个单态化实例的参数类型，到 codegen 那
  一步的时候，已经是普通的具体类型了（比如 `print_area(const Shape
  auto&)` 拿 `Circle` 实例化之后，就是精确的
  `print_area(const Circle&)`）——[§11.9](ch11-modules-and-libraries.md)
  已有的参数类型编码方案，天然就能让每个不同的实例化拿到不同的
  mangled 符号，跟它已经能区分普通重载是同一个道理。
- **这一轮明确不做的**：函数体内的递归拆包（跟上面的 fold expression
  相对）、函数模板的显式特化、模板模板参数、默认模板实参、关联类型，
  以及**通用**动态分发 / 对象安全接口擦除（scpp 对应虚函数/`dyn`
  的那部分，跟继承一起搁置）。泛型 `struct`/`class` 类型，之前整个
  搁置，现在已经设计出来了——见 [§5.14](#514-泛型类型)。但更窄、只
  针对可调用对象的拥有型擦除层（`std::function`/
  `std::move_only_function`）会在
  [§5.18](#518-类型擦除调用包装器stdfunction-与-stdmove_only_function)
  单独设计。

## 5.12 闭包（Lambda 表达式）

原样复用真实 C++ 的 lambda 语法——`[capture-list](params) { body }`，
包括 `mutable`、尾置返回类型、以及泛型（C++14 起的 `auto` 参数）
lambda。一个 lambda 表达式的类型，跟真实 C++ 完全一样，是一个匿名的、
编译器合成的类：每个被capture的名字对应一个成员，外加一个实现函数体的
`operator()`。这不是什么新概念——就是真实 C++ 本来就在做的那套脱糖——
所以现有的 `struct`/`class` 规则（[§4](ch04-struct-vs-class.md)）能
原样套用到闭包的成员上，不需要任何新机制：

- **按值capture**（`[x]`，或者 init-capture `[x = expr]`）是一个普通的
  拥有型成员，copy或者move进来，跟从一个参数初始化一个`class`成员
  （[§4.2](ch04-struct-vs-class.md)）完全一样，遵循跟别处一样的move
  规则（[§5.1](#51-所有权与移动move--ownership)）。move-only类型要进
  闭包，靠的就是init-capture，跟传给构造函数是一回事：`std::unique_ptr<T> p`
  就写`[p = std::move(p)]`。
- **按引用capture**（`[&x]`）是一个引用类型的成员——`struct`不允许，
  但`class`允许（[§4.1](ch04-struct-vs-class.md)/
  [§4.2](ch04-struct-vs-class.md)）——所以这个闭包值本身就变成了一个
  生命周期追踪的值，跟`class`持有一个`T&`/`const T&`字段、或者
  `std::span`是一回事。它跟其它任何持有引用的值一样，参与同一套
  alias-XOR-mutability和悬空检查（[§5.1](#51-所有权与移动move--ownership)-
  [§5.3](#53-生命周期lifetime)）：闭包活着的时候，被借用的局部变量不能
  被move、不能被重新赋值、也不能离开作用域。按引用capture不止一个名字
  （`[&a, &b]`）时，闭包自己的生命周期跟这几个变量共同绑定——就是
  [§5.3](#53-生命周期lifetime)已经给"好几个没分组的引用参数"用的那套
  保守默认分组处理。
- **`[=]`/`[&]`（整体隐式capture），以及`[&, x]`/`[=, &y]`这种混合
  写法，原样接受**——scpp不要求每个capture都得单独点名。真实C++本身
  并不把整体capture当成普遍要避免的坏味道，所以scpp不会加一条真实
  C++自己都不要求的限制。
- **例外：`this`/`*this`必须显式capture。** 裸的`[=]`或者`[&]`因为
  lambda写在方法里、用到了成员变量而隐式capture了`this`，这是
  **编译错误**——得写`[this]`、`[*this]`、`[=, this]`或者`[&, this]`。
  真实C++20只是把这个**标记为deprecated**（P0806R2），因为`[=]`隐式
  capture `this`这件事本身就很有误导性：看起来像是把整个receiver都
  复制了一份，实际上capture的是一个指向它的裸指针——这是真实存在、有
  据可查的use-after-free bug来源，一旦闭包活得比对象还长就会出问题。
  scpp这里直接做成硬错误，不是deprecation warning，跟本书对待其它
  已经被认定的C++坑的一贯做法一致（比如[§6](ch06-safe-subset.md)禁止
  裸`unsigned`）。这条规则目前实际上是休眠的，因为class方法体本身的
  完整检查还没设计出来（[§4.2](ch04-struct-vs-class.md)；
  [§5.9](#59-方法与-this)只覆盖了目前已经检查的部分）——现在先定下来，
  这样等方法体拿到完整检查能力那天，这条规则已经是现成的，而不是到
  时候才发现的一个漏洞。

**调用一个闭包**（`c(args)`）就是对它（编译器合成的）`operator()`的
一次普通调用，跟任何别的方法调用（[§5.9](#59-方法与-this)）检查方式
一样——没有任何闭包专属的东西。

**把闭包传给别的函数**，有两种不同形状。零开销路径是用 concept 约束的
泛型参数（[§5.11](#511-泛型函数与-concept)），按具体闭包类型单态化：

```cpp
template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

void for_each_doubled(std::span<int> s, IntConsumer auto&& f) {
    for (int i = 0; i < s.size; ++i) f(s[i] * 2);
}
```

这依然是算法式代码的默认选择。而拥有型、类型擦除那条路，则是
[§5.18](#518-类型擦除调用包装器stdfunction-与-stdmove_only_function)
里的 `std::function<Sig>` / `std::move_only_function<Sig>`：只有当你真正
想表达的是"存、返回、或者异构地拥有某个这个签名的可调用对象"，而不是
把具体闭包类型保留到编译期时，才用它们。

**不需要任何新机制，就能阻止一个按引用capture的闭包"逃逸"**——比如
被传给另一个函数、然后存进一个全局数组里。
[§5.3](#53-生命周期lifetime)的intraprocedural模型，以及
[ch11 §11.9](ch11-modules-and-libraries.md#119-健全性跨模块检查器只需要签名)
对它的重申，已经确立了：调用方只需要被调用函数的**签名**，永远不需要
看它的函数体——如果某个函数自己的函数体把闭包参数存进了`'static`
时长的存储里，那这个函数自己的签名必须早就承诺了它的参数活得够久，
这件事在这个函数自己定义的地方就检查过一次了——不然这个函数自己压根
编译不过。一个绑定在短生命周期局部变量上的闭包，天然满足不了这样的
签名，所以把它传给那个函数，在调用点就会被拒绝，调用方自己的检查器
完全不需要去看被调用函数的函数体。

## 5.13 生命周期泛型参数（Lifetime-Generic Parameters）

让库代码能接受一个闭包，通过一条普通的 `concept` 验证"这个闭包自己的
参数对**任意**生命期都能安全调用"，然后真的拿一个这个库函数自己内部
现造出来的值去调用它——这个值的生命期，闭包的作者写这个闭包的时候根本
不可能提前知道。这是对 [§5.3](#53-生命周期lifetime) 已有的具名 group
机制、以及 [§5.11](#511-泛型函数与-concept) 的 concept 约束泛型，做的
一个小范围、有针对性的扩展——一个保留的 group 名字，加一条新的
concept 检查语义，没有新语法。它让 Rust `thread::scope`那种形状的 API
（回调函数自己参数的生命期，是被调用方而不是调用方决定的）能写成普通
库代码，不需要像 `std::thread`/`std::span`那样被硬编码进编译器——
基于这个机制去设计那套线程库 API 本身，是另外的后续工作，不属于这份
语言层面的定义。

- **`[[scpp::lifetime(generic)]]` 是一个保留的 group 名字。** 这样标记
  一个引用型参数，等于给它分配一个全新的、编译器合成的 group——程序里
  任何别的地方都不可能跟它 unify，因为 `generic`根本不是任何用户代码能
  拼写出来或者复用的 group 名字。这不需要任何新的函数体检查逻辑——就是
  [§5.3](#53-生命周期lifetime) 已有的"不同 group 之间互相独立，除非
  显式 unify"规则，套用在一个别人永远拼不出名字的 group 上而已。有个
  推论是这条已有规则本身就带出来的，不需要另外声明：在函数/闭包自己的
  函数体内部，一个标了 `generic`的参数（或者从它派生出来的东西）可以
  做普通的、同步的操作——读它、在它上面调用方法、把它传给另一个接受
  兼容约束的函数——但永远没法写进这个函数自己的按引用 capture、任何
  其它具名 group、这个函数的返回值、或者全局/static 存储里：没有任何
  合法的 group 能让它 unify 进那些地方。
- **`requires`表达式自己的探测参数也可以带同样的 tag。** 给一条
  compound requirement 的探测参数标上 `[[scpp::lifetime(generic)]]`，
  会改变这条 requirement 检查的内容：
  ```cpp
  template<typename T>
  concept AcceptsToken = requires(T a, Token& tok [[scpp::lifetime(generic)]]) {
      { a(tok) } -> std::same_as<void>;
  };
  ```
  只有当 `T`自己那个对应的 call-operator/函数参数，在它**自己定义的
  地方**也标了 `[[scpp::lifetime(generic)]]`，这条 requirement 才算
  满足——不再只是"能不能拿某个 `Token&`去调用"这么简单。这是给一个
  本来就合法的 C++20 语法位置（`requires`表达式的参数本来就可以带
  attribute，真实编译器本来就会接受并且忽略它不认识的 attribute，见
  [ch00](ch00-design-philosophy.md) §2）加上的新语义——真实 C++20 的
  concept 完全没有"检查一个参数的 lifetime group"这种能力，所以这是
  scpp 专属的语义，不是新语法。
- **调用点的豁免规则。** 一旦通过这样一条 concept（或者因为这个具体
  可调用对象自己的声明就摆在眼前直接可见）确认了某个值的类型带有一个
  标了 `generic`的参数，拿**任意**具体实参去调用它就无条件放行，不管
  这个实参属于哪个 group——哪怕这个引用的生命期是在**调用方自己**的
  函数体内部现造出来的（被调用方根本不可能提前给它起名字）：
  ```cpp
  void with_fresh_token(AcceptsToken auto&& f) {
      Token tok;   // 就在这个函数自己的函数体里现造出来的
      f(tok);      // 允许：f 自己的参数被声明成了 lifetime-generic
  }
  ```
  正常情况下，传一个引用型实参，要求它满足对应参数所在的 group（见
  [§5.3](#53-生命周期lifetime)）；这是对这条规则**唯一**的豁免，之所以
  站得住，正是因为前两条规则已经保证了被调用方自己的函数体不可能做
  任何要求这个实参活得比这一次调用还久的事。

这样就达成了真实 Rust `std::thread::scope`靠一个 higher-ranked trait
bound（`for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> T`）才能拿到
的那种健全性，而且没有把"对生命期做泛型"变成一个独立的一等公民范畴：
[§5.3](#53-生命周期lifetime) 当初就已经放弃了 `'a`那种生命周期语法，
选了更简单、非泛型的具名 group 机制，这三条规则是在扩展同一套机制，
不是推翻当初那个选择——只是补上了"回调函数参数的生命期由被调用方
而不是调用方决定"这一种、纯粹的分组机制原本够不着的场景。

## 5.14 泛型类型

把 [§5.11](#511-泛型函数与-concept) 对编译期多态的答案，从函数扩展到
`struct`/`class`定义本身——原样复用真实 C++ 模板语法
（`template<typename T> class X { ... };`），支持多个类型参数和参数包，
延续 §5.11 定下的同一条"concept 可选"原则，只是这次是**按成员**分开
应用，不是整个类一次性绑死：

- **泛型类型自己的类型参数可以裸写**——跟裸写的泛型函数参数
  （§5.11）一个道理，意味着"除了每个 scpp 类型天然就有的通用底线
  操作（move、存成字段、传给兼容的参数、return——见
  [§5.1](#51-所有权与移动move--ownership)）之外，不保证任何东西"。
  这足够声明裸类型的字段、围着它写普通的构造函数/访问器；但不够对
  这个类型调用任何方法，或者对它用任何运算符。
- **每个方法（包括构造函数）可以各自加一条 `requires Concept<T>`**，
  原样复用真实 C++20 语法，解锁的东西**只对这一个方法自己的函数体
  生效，在这个方法自己定义的地方一次性检查**——跟泛型函数同一条
  "定义处一次性检查"原则，只是分摊到每个成员上，不是绑在整个类共享
  的一条约束上：
  ```cpp
  template<typename T>
  class Vec {
      T item;
  public:
      Vec(T x) : item(std::move(x)) {}

      void push(T x) { item = std::move(x); }         // 不需要 T 支持任何操作

      bool less_than(const T& other) const requires std::totally_ordered<T> {
          return item < other;                          // 只有这一个方法需要比较
      }
  };
  ```
  用 `Vec<SomeType>`的调用方，不管 `SomeType`支不支持别的操作，都能调
  `push`；但只有 `SomeType`真的满足 `std::totally_ordered`，才能调
  `less_than`——这个检查、以及报错，都发生在调用点，给出精确的诊断，
  不是拖到实例化才冒出来的那种看不懂的报错。
- **泛型 `struct`的类型参数不能裸写**——跟 `class`不一样，`struct`的
  字段必须**全部**是 trivial 类型（[§4.1](ch04-struct-vs-class.md)），
  而"trivial 与否"是整个类型的布局/ABI 性质，没法像 class 那样分摊到
  各个方法自己去分别保证（`struct`本来就没有方法可以拿来分摊）。所以
  泛型 `struct`的类型参数，必须在 `struct`自己这一层，就用一个能保证
  trivial 的 concept 约束住。
- **参数包（`typename... Ts`）支持，用来构建"自身成员布局随参数个数
  变化"的类型**（比如 `Tuple`这种），但只能靠**递归继承**实现——真实
  C++ 没有能把 pack 直接展开成成员列表的语法（`Ts... elems;`这种写法
  已经作为提案 [P1858](https://wg21.link/P1858) 提出来了，但到 C++23
  都没有被任何标准采纳；实测验证过当前编译器会拒绝这个写法，而 scpp
  的可擦除原则（[ch00](ch00-design-philosophy.md) §2）要求每一种写法
  都得能被一个真实的、未修改的 C++ 编译器接受）。泛型类型支持的变参
  模式**只有两种固定的**——不是任意/通用的特化：
  ```cpp
  template<typename... Ts> class Tuple;

  template<> class Tuple<> {};   // base case：空 pack

  template<typename Head, typename... Tail>
  class Tuple<Head, Tail...> : private Tuple<Tail...> {   // 递归情形
      Head head;
  };
  ```
  base case 是**空** pack（`Tuple<>`），不是"剩一个元素"——pack 匹配
  零个实参本来就是标准、普通的 C++（[temp.variadic]，C++11 起），不是
  scpp 发明的特例；真实 `std::tuple`的实现（直接查证过 libstdc++）反而
  是在"剩一个元素"那里就停止递归，纯粹是它们自己实现上的一个小优化
  （省掉一层本来就是空的基类）——C++ 标准根本没有规定 `std::tuple`
  内部用哪种布局（只要求 `get`是常数时间，见下面），所以 scpp 可以自由
  选择更简单、统一的空 pack 这种 base case。
- **非类型模板参数支持，限定成标量类型**（[§6](ch06-safe-subset.md)的
  标量家族：`bool`、各种定长整数、`char`、`float32_t`/`float64_t`、
  `size_t`、`ptrdiff_t`及其别名）——不包含 `struct`/`class`，这样就
  彻底绕开了真实 C++20 的"structural type"那一整套机制（按成员逐个
  比较来判断模板实参是否相等），反正目前也没有场景需要 class 类型的
  非类型参数。两个非类型实参算不算同一个模板实参，规则是**位模式是否
  完全一致**——这一条规则对所有标量类型都统一成立：对整数来说，这就
  退化成普通的值相等（整数的标量表示没有冗余，C++20 起还强制规定
  补码表示，更加没有例外）；对浮点数来说，这条规则拿真实编译器验证过，
  就是真实 C++ 自己对浮点非类型实参用的**那条**规则（`0.0`和 `-0.0`
  按 `==`相等，但位模式不同，**不算**同一个模板实参；两个独立算出来、
  位模式完全相同的 `NaN`，即使 `NaN == NaN`为假，也**算**同一个模板
  实参）——所以这是同一条规则统一覆盖两种情况，不是分两条规则，也不是
  scpp 新发明的规则。
- **对递归定义的变参类型做按下标访问，复用"从基类推导模板实参"这个
  机制**——真实、标准的 C++ 行为（[temp.deduct.call]），不是 scpp 专门
  发明的机制：如果一个函数模板的参数类型是某个类模板的具体特化，而
  实参的类型在继承链里能找到唯一一层匹配这个特化模式的基类（直接或者
  间接），编译器就会拿那一层基类的实际模板实参，反推出函数自己还没
  确定的模板参数。在上面那个递归定义里穿一个标量、非类型的下标进去
  （跟真实 `std::tuple`内部实际实现方式一致——直接查证过 libstdc++的
  `<tuple>`），就能让访问函数一步到位推导出该访问继承链的哪一层——
  拿到常数时间的按下标访问（这正是 C++ 标准对 `std::get`唯一做的
  保证），访问函数自己完全不需要递归：
  ```cpp
  template<size_t Idx, typename... Ts> class TupleImpl;

  template<size_t Idx> class TupleImpl<Idx> {};   // 空 pack：递归的自然终点

  template<size_t Idx, typename Head, typename... Tail>
  class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {
  public:
      Head value;
  };

  // 完整 header 形式（见 §5.11）——这里必须用，因为 Head/Tail 是从
  // 基类推导出来的，I 又是调用方显式指定的（get<2>(t)），两者都不是
  // 简写的 auto 占位符能表达的。
  template<size_t I, typename Head, typename... Tail>
  Head& get(TupleImpl<I, Head, Tail...>& t) { return t.value; }
  ```
- **这一轮明确不做的**：任意/通用的模板特化（上面那种固定的空
  pack/Head+Tail 模式除外）、模板模板参数、默认模板实参、class 类型的
  非类型模板参数，以及关联类型。

## 5.15 线程安全的结构性属性：`[[scpp::thread_movable]]`、`[[scpp::thread_shareable]]` 与 `[[scpp::thread_movable_if(a, b)]]`

让库代码（比如一个负责创建线程的函数）能通过一个普通的 attribute 标在
参数上去要求"传给我的这个东西，真的能安全地跨越一个真实的并发边界使用"。
这里涉及的底层性质，仍然只有 thread-movable 和 thread-shareable 这两个；
但 scpp 把它们暴露成了三种表面形式：

- 参数/类型声明 attribute：`[[scpp::thread_movable]]` 和
  `[[scpp::thread_shareable]]`；
- 条件式类型声明 attribute：`[[scpp::thread_movable_if(a, b)]]`；
- 编译器内置谓词：`scpp::is_thread_movable(T)` 和
  `scpp::is_thread_shareable(T)`。

施加到一个泛型函数的参数上时，`[[scpp::thread_movable]]` 或
`[[scpp::thread_shareable]]` 都会约束这个参数（可能是模板推导出来）的类型
必须满足对应的性质，就跟把 `[[scpp::lifetime(name)]]` 叠在参数上
（[§5.3](#53-生命周期分组lifetime-groups)）是一回事；改成施加到一个
`struct`/`class` 自己的声明上时，这些 attribute 就变成了库作者对该类型
线程安全契约的显式背书，覆盖掉下面结构化推导规则本来会得出的结论——
对应 Rust 里显式写出来的 `unsafe impl Send`/`unsafe impl Sync`。

这两个内置谓词是**编译器 intrinsic**，不是普通函数调用：它们在括号里接收
的是一个**类型名**，写法跟真实 C++ 的 builtin trait 一样（`__is_trivially_copyable(T)`
这种风格），并且可以出现在任何要求布尔常量表达式的地方。对任意类型 `T`
来说，`scpp::is_thread_movable(T)` 的含义是："先看 `T` 自己身上有没有覆盖；
如果有，就取那个覆盖值；如果没有，就取下面结构化推导算出来的结果"。
`scpp::is_thread_shareable(T)` 对 thread-shareable 性质也完全同理。

- **`[[scpp::thread_movable]]`** 回答的问题是：这个类型的一个值，能不能
  按值交给一个新起的线程，交出去之后原来那个线程完全碰不到它了？（对应
  Rust 的 `Send`。）
- **`[[scpp::thread_shareable]]`** 回答的问题是：两个或者更多线程同时各自
  持有一个指向同一个对象的 `const T&`，安不安全？（对应 Rust 的
  `Sync`。）
- **结构化推导规则**（没有在类型自己身上手动覆盖时的默认行为），递归
  计算：
  - 每个标量类型：两个性质都成立。
  - 带引用成员的类型（`T&`/`const T&`/`T&&`——不管是普通 `class`的字段，
    还是闭包按引用 capture 的东西，见
    [§5.12](#512-闭包lambda-表达式)）永远不是 thread-movable——把
    这样的值"移动"给另一个线程，并不会转移被引用对象本身的所有权，
    所以这么做不像移动一个拥有型的值那样，能让原来的线程碰不到
    被引用的东西。
  - `struct`/`class`（不带引用成员）：thread-movable 和
    thread-shareable 各自成立，当且仅当每个成员自己都具备这个性质——
    **除了** `mutable`成员（[§4.2](ch04-struct-vs-class.md)），它会
    让整个类型是 thread-movable 但**永远不是** thread-shareable
    （对应 Rust 的 `Cell<T>`：一次只交给一个线程独占没问题，但两个
    线程同时通过一个"看起来共享"的引用去读，就不安全了，因为
    `mutable`本来就是官方认可的、通过看似只读的引用去写的办法）。
  - 裸指针 `T*`：默认两者都不成立——这跟裸指针本来就需要为检查器
    验证不了的东西背书是一致的（[§5.5](#55-禁止项除非在-scppunsafe-里)）；
    要背书就把它包进一个标记了 `[[scpp::thread_movable]]`/
    `[[scpp::thread_shareable]]` 的 `struct`/`class` 里，见下面。
  - 闭包（[§5.12](#512-闭包lambda-表达式)）：thread-movable 成立，
    当且仅当它完全没有按引用 capture（见上面），而且每个按值 capture
    的成员自己的类型都是 thread-movable。thread-shareable 成立，
    当且仅当它没有按**可变**引用 capture 的东西（多个线程同时通过一个
    共享的可变引用去调用，可能产生竞争），每个按值 capture 的成员
    类型都是 thread-shareable，而且每个按**const**引用 capture 的
    成员，它指向的那个类型也是 thread-shareable。
- **在类型声明上手动覆盖**：一个 `struct`/`class` 可以直接在自己的定义
  上标记 `[[scpp::thread_movable]]` 和/或 `[[scpp::thread_shareable]]`：
  ```cpp
  struct [[scpp::thread_movable]] RawBufferHandle {
      int* data;
      int len;
      // 作者已经验证过这个类型自己的不变式，让它交给另一个线程是健全的，
      // 哪怕上面那条结构化规则会说"不行"——因为它带了一个裸指针
  };
  ```
  这会彻底覆盖掉这个类型的结构化推导结果——完全对应 Rust 的
  `unsafe impl Send for RawBufferHandle {}`——也是唯一能让一个编译器
  自己验证不了的类型（最常见的情形就是带一个裸指针的类型）参与这两个
  性质的办法。
- **在类型声明上做条件式覆盖**：一个（通常是泛型）`class` 也可以改为
  声明 `[[scpp::thread_movable_if(a, b)]]`，其中 `a` 和 `b` 是两个按
  每次实例化分别求值的布尔常量表达式。第一个实参成为这个类型自己的
  thread-movable 值；第二个实参成为它自己的 thread-shareable 值。它们
  两个加在一起，会替换掉这个类型该次实例化原本根据字段结构自动推导
  出来的结果。最常见的写法，就是用上面那两个内置谓词去描述类型参数：
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T),
      scpp::is_thread_shareable(T)
  )]] MyOwningBox {
      T* ptr;
  };
  ```
  这就是表达下面这类意思的通用机制："我的内部表示里包含了某些单看结构
  不足以让编译器推出真实并发不变式的东西（例如裸指针）；但作为类型作者，
  我可以把那个不变式显式写出来。"
- **`std::unique_ptr<T>` 就是这个通用机制的一个例子。** 它的声明可以显式
  写成：
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T),
      scpp::is_thread_shareable(T)
  )]] unique_ptr {
      // ...
  };
  ```
  这**不是**编译器对 `std::unique_ptr` 这个库类型名字做的硬编码特判；
  它只是普通库代码在使用任何用户自定义拥有型盒子都能使用的同一套
  attribute。这里之所以需要覆盖，是因为 `unique_ptr` 的裸指针字段，
  光看结构，跟一个任意别名化的裸指针没有区别；真正更强的不变式
  ——"它是唯一 owner"——是库作者显式背书出来的，所以把整个 `unique_ptr`
  交给另一个线程时，那份独占访问权也跟着一起转移。这个思路对应 Rust
  对唯一拥有型堆指针显式写条件 `Send`/`Sync` impl 的做法。
- **`std::shared_ptr<T>` 也是一个例子，但它不是独立转发，而是合取规则。**
  它的声明可以写成：
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T) && scpp::is_thread_shareable(T),
      scpp::is_thread_movable(T) && scpp::is_thread_shareable(T)
  )]] shared_ptr {
      // ...
  };
  ```
  同样，这只是普通库声明在使用通用覆盖机制，不是编译器给某一个类型名字
  的魔法规则。这里必须取合取：把一个 `shared_ptr` 的 handle 移到另一个
  线程，并不会收回其它还活着的 handle 的访问权，所以 pointee 本身必须
  已经既能安全跨线程移动，也能安全跨线程共享。
- **约束一个参数**：一个负责创建线程的函数，把这两个 attribute 之一
  直接标在它的闭包参数上，就跟标 `[[scpp::lifetime(name)]]` 一样：
  ```cpp
  template<typename T>
  void spawn(T&& f [[scpp::thread_movable]]) {
      // ...
  }
  ```
  编译器在每次调用 `spawn` 时都会检查：`f` 这次调用推导出来的实参类型
  是不是 thread-movable（靠上面的结构化规则，或者靠它自己身上的手动
  覆盖）——不满足就是不合法（ill-formed），跟别的参数 attribute 违反
  时的待遇完全一样。
- **按引用 capture 该有的安全性，用的是已经有的机制。** 按**可变**
  引用 capture（`[&x]`绑定一个非 `const`的 `x`）本来就是一个独占借用
  （[§5.2](#52-借用与别名borrow--aliasing)）：光是别名 XOR 可变这条
  规则，就已经保证了不会有别的线程同时在碰 `x`，这也是为什么
  thread-movable/thread-shareable 不需要、也没有对它做额外约束。
  按**共享**（`const`）引用 capture 就不一样了，它可以跟同一个根的
  其它共享借用共存，包括别的线程持有的共享借用（比如两个不同的
  scoped thread，各自从同一个外层作用域按 `const T&`capture 同一个
  东西）——这正是 thread-shareable 要回答的场景。

## 5.16 函数指针（Function Pointers）

照抄真实 C/C++ 的函数指针语法，只加一个东西：一个指向函数的指针类型，
要么是 *unsafe-qualified*，要么不是，这一点被当成类型自身的一部分来
跟踪——通过它调用是否需要 unsafe context，完全类比 Rust 自己的 `fn` 与
`unsafe fn` 指针类型的区分（在
[规范正文 §5.2](../../spec/zh/01-unsafe.md#52-函数指针类型function-pointer-typesdclptrscppunsafe)
里做了形式化）。

- **语法拼写：跟真实 C++ 完全一样。** `RetType (*p)(ParamTypes...)`
  声明 `p` 为一个指向函数的指针；一个普通函数名，或者 `&函数名`，会
  decay 成这种类型的值（[expr.unary.op]/[conv.func]），跟真实 C++
  一模一样。这里不需要任何新语法。
- **为什么单纯的函数指针类型不够用。** [§1.2](ch01-safety-context.md)
  已经允许一个函数自己的声明 opt-in 调用点管制（`[[scpp::unsafe]]
  RetType f(...)`）——调用 `f` 就需要 unsafe context。如果取 `f` 的
  地址造出来的是一个普通的、不受管制的指向函数的指针值，把它存进一个
  看起来很"普通"的变量、再通过这个变量调用，就会悄悄绕开这道关卡——
  这正是 [§5.5](#55-禁止项除非在-scppunsafe-里) 那份清单在别处要堵上的
  同一种风险。所以指针自己的类型也必须携带同样的义务。
- **标记类型的写法：`[[scpp::unsafe]]` 写在 `*` 后面。**
  ```cpp
  int (* [[scpp::unsafe]] up)(int, int);  // unsafe-qualified
  int (*                  sp)(int, int);  // 不是 unsafe-qualified（默认）
  ```
  这是真实 C++ 语法本来就给指针声明符留的一个 attribute 位置
  （`T* [[attr]] p;`），所以跟 `[[scpp::unsafe]]` 另外那两个位置一样
  （[§1.3](ch01-safety-context.md)），不引入任何新语法。另外两个候选
  位置都考虑过，也都被否掉了：紧贴在 `*` 前面（MSVC 的 `__stdcall`
  这类调用约定关键字写的位置）压根不是真实 `[[...]]` attribute 语法
  能出现的位置（验证过：clang 直接报错"an attribute list cannot
  appear here"）；写在参数列表后面（按 [§1.3](ch01-safety-context.md)
  自己那条注释，会附着到*函数类型*上）虽然也能编译过，但最终选了
  直接标在"指针"本身、写在它当下所在的位置，这样更直白。
- **取函数地址时，类型自动确定。** 一个普通函数——自己的声明没被标记
  `[[scpp::unsafe]]`——的地址，是一个不是 unsafe-qualified 的指针值。
  一个自己的声明**就是** `[[scpp::unsafe]]` 标记的函数，或者一个没有
  函数体的 `extern "C"` 声明（[§2.1](ch02-boundary-rules.md)，本来就
  受一样的管制，[§5.5](#55-禁止项除非在-scppunsafe-里)）的地址，是一个
  unsafe-qualified 的指针值。取地址这个动作本身不需要额外标注——是哪种
  类型，取决于被指向的是哪个函数，跟指针变量自己怎么写没关系。
  ```cpp
  [[scpp::unsafe]] int get_unchecked(int* base, int index) { return base[index]; }
  int add(int a, int b) { return a + b; }

  int (* [[scpp::unsafe]] up)(int*, int) = get_unchecked;  // OK
  int (*                  sp)(int, int)  = add;            // OK
  int (*                  bad)(int*, int) = get_unchecked; // 不合法：
                                    // get_unchecked 的地址是 unsafe-qualified
                                    // 的，bad 不是
  ```
- **单向隐式转换：只能从不是 unsafe-qualified 转成 unsafe-qualified。**
  一个不是 unsafe-qualified 的、指向函数的指针值，总能存进一个
  unsafe-qualified 的指针变量（无害——这只会*放宽*调用方的义务，
  不会撤销一个本来就有的义务）；反过来会被拒绝。这跟真实 C++17 自己对
  `noexcept` 函数指针的规则是同一回事（一个指向 `noexcept` 函数的
  指针可以转换成普通指针，反过来不行，[conv.fctptr]）——完全一样形状
  的规则，只是应用在了不同的承诺上。
  ```cpp
  int (* [[scpp::unsafe]] up2)(int, int) = add;   // OK：放宽方向
  int (*                  sp2)(int*, int) = get_unchecked;  // 不合法
  ```
- **通过指针调用。** 通过一个不是 unsafe-qualified 的指针调用，是一个
  普通的、不受管制的操作——跟按名字调用一个普通函数完全一样，因为按
  构造，只有普通函数的地址才可能存进这种指针。通过一个 unsafe-qualified
  的指针调用，加入了 [§5.5](#55-禁止项除非在-scppunsafe-里) 那份清单：
  需要 unsafe context，跟按名字调用一个 `[[scpp::unsafe]]` 标记的函数
  待遇一样。
  ```cpp
  int r1 = up(base, 0);                   // 不合法：在 unsafe context 之外
  int r2;
  [[scpp::unsafe]] { r2 = up(base, 0); }  // OK
  int r3 = sp(1, 2);                      // OK：sp 永远不会是 unsafe-qualified
  ```
  这是一条真正新增的 gated operation，不是在重述已有的一条：
  [§5.5](#55-禁止项除非在-scppunsafe-里) 里已有的"调用一个标记
  `[[scpp::unsafe]]` 的函数"这一项，只覆盖按名字直接调用的情形——通过
  指针调用时，*postfix-expression* 指代的是指针值，不是函数本身，所以
  需要单独一条规则。
- **调用不是解引用。** `fp(args)`（或者等价的 `(*fp)(args)`，同样合法，
  跟真实 C++ 一样）从来不会触发 [§5.5](#55-禁止项除非在-scppunsafe-里)
  那条裸指针解引用的管制：那条管的是*通过*指针读数据
  （[§5.7](#57-取地址expr与裸指针)），而在某个地址上调用代码是一个
  不同的操作，规则见上面。所以一个不是 unsafe-qualified 的函数指针，
  不管怎么写，调用它都不需要 `[[scpp::unsafe]]`。
- **跟别的裸指针一样是 trivial 的。** 函数指针（不管哪种）不带编译器
  跟踪的生命周期，也从不参与 move/借用检查
  （[§5.1](#51-所有权与移动move--ownership)-[§5.2](#52-借用与别名borrow--aliasing)）
  ——跟 `T*` 已有的待遇一样（[§4.1](ch04-struct-vs-class.md)）。可以
  自由复制，也能合法地当 `struct` 成员。
- **补上 [§5.10](#510-函数重载) 留的坑**：`&重载名字` 现在有规则了——
  见那里。
- **这一轮明确不做的**：pointer-to-member-function
  （`RetType (Class::*)(ParamTypes...)`），它还会跟
  [§5.9](#59-方法与-this) 的"`this` 当成借用参数"模型产生尚未设计的
  交互；以及一个返回函数指针的函数，这种情况下真实 C++ 自己的声明符
  语法会让*外层*函数的前置和后置 attribute 位置互相产生歧义。这两个
  目前都还没有逼着必须解决的实际用例。

## 5.17 解引用运算符作用于 `class`

scpp 允许一个 `class` 把 `operator*()` 声明成一个普通的、非 static 成员
函数，支持的两种有用形状就是：

```cpp
class Box {
public:
    T& operator*();
    const T& operator*() const;
};
```

这里最重要的是 scpp **没有**为它额外发明什么：没有新的借用规则，没有新
的所有权例外，也没有单独的一套 `operator->` 协议。编译器只是把 `*x`
当成一次普通方法调用的语法糖，再把 `x->y` 保持成原来那套 `(*x).y`
语法糖——以前这是给指针类类型用的，现在用户自定义 `class` 也能接进来。

- **`*x` 就是一条普通方法调用。** 如果 `x` 的类型是某个 `class`，而这个
  `class` 声明了匹配的 `operator*()`，那么 `*x` 的检查方式，跟用户直接
  写 `x.operator*()` 完全一样。可变 receiver 选非 `const` 重载，`const`
  receiver 选 `const` 重载，跟别的方法调用共用同一套 receiver-借用规则
  （[§5.9](#59-方法与-this)）。
- **返回值就是一条普通的"方法返回引用"。** 因为支持的形状是返回 `T&`
  或者 `const T&`，所以现有"一个方法返回引用"的全部规则都原样适用：
  借用 `*x` 会把借用记在 `x` 的根对象上，而只要那个引用还活着，move 或
  重新赋值 `x` 都会被拒绝，理由跟别的任何方法返回引用完全一样
  （[§5.3](#53-生命周期lifetime)/[§5.9](#59-方法与-this)）。例如
  `int& r = *b; Box c(std::move(b));` 被拒绝，不是因为有一条特殊的
  "`operator*` 规则"，而只是复用了现成的"借用活着时不能 move"。
- **`x->y` 依然只是 `(*x).y` 的语法糖。** 所以一旦 `*x` 能在用户自定义
  `class` 上工作，`x->field` 和 `x->method()` 就自动跟着工作了，不需要
  第二套设计。`std::unique_ptr<T>` 的行为保持原样；现在只是用户自定义
  `class` 也能加入同一个表面语法。
- **没有单独可重载的 `operator->`。** scpp 不提供真实 C++ 那套单独的
  `operator->` 协议。光靠上面这条 `(*x).y` 重写，语言已经拿到了有用的
  `x->y` 拼写；再加一个第二运算符名字，只会给同一个受借用检查约束的
  效果再开一条入口。
- **当前范围故意收得很窄。** 这是一个 `class` 特性，不是 `struct`
  特性；新增的运算符名字只有 `operator*`。像 `operator+` 这种其它运算
  符名字依然不在这一轮范围内；`operator=` 则继续由
  [§4.2](ch04-struct-vs-class.md) 里普通的 copy 赋值规则单独管。

## 5.18 类型擦除调用包装器：`std::function` 与 `std::move_only_function`

scpp 提供两个拥有型、类型擦除的可调用对象包装器，沿用真实 C++ 的这组
分工：

- **`std::function<Sig>`**：存一个签名为 `Sig` 的可调用目标，但这个目标
  必须是可 copy 构造的。
- **`std::move_only_function<Sig>`**：存一个签名为 `Sig` 的可调用目标，
  目标即使是 move-only 也可以。

这里最关键的设计点，是这两个包装器**不**是什么：它们不是老
`unique_ptr`/`span`/`expected` 那种深度编译器内建。一旦泛型 `class`
模板能把需要的表面能力表达出来——多个模板参数、对函数类型模板实参做
偏特化、泛型构造函数、以及方法里带名字的参数包——这两个包装器就是普通
库代码。它们唯一需要的编译器 intrinsic 级帮助，只是把一个函数类型模板
实参（比如 `int(int, int)`）拆成"返回类型 `int` + 参数类型包 `(int, int)`"；
这跟真实 C++ 里对 `R(Args...)` 做偏特化模式匹配扮演的是同一个角色。

```cpp
template<typename Sig>
class function;

template<typename R, typename... Args>
class function<R(Args...)> {
    // ...
};

template<typename Sig>
class move_only_function;
```

**这两个包装器都没有 null / empty 状态。** 这是刻意偏离真实 C++ 的
`std::function`/`std::move_only_function`：真实 C++ 里它们可以默认构造、
可以用 `operator bool` 测是不是空、也可能处于 empty 状态。scpp 不要
这个设计：一旦一个 `function<Sig>` 或者 `move_only_function<Sig>` 对象
存在，它就总是拥有一个有效、可调用的目标。如果程序需要"这里可能没有
回调"这种可选性，就把它写进类型里：

```cpp
std::optional<std::function<void(int)>> maybe_callback;
std::optional<std::move_only_function<void()>> maybe_job;
```

这样"这里可能没有 callable"就是显式的类型信息，而不是藏在一个名义上
拥有目标的包装器内部当哨兵状态。移动一个包装器时，也因此直接复用
[§4.2](ch04-struct-vs-class.md) 里普通 `class` 的规则：目标被 move 到
目标包装器里，源包装器对象自己变成一个普通的 moved-out 对象，而不是一个
特殊的、还能继续拿来调用但内部为空的 callable 盒子。

**`std::move_only_function` 从一开始就支持 cv/ref-qualified 签名。** 例如：

```cpp
std::move_only_function<void() const> f1;
std::move_only_function<void() &>     f2;
std::move_only_function<void() &&>    f3;
```

这个限定符属于"被存进去的 callable 的 call-operator 契约"，跟真实
C++23 一样：实例化成 `void() &` 的包装器，承诺目标可以作为左值调用；
实例化成 `void() &&` 的包装器，承诺目标可以作为右值调用。

**允许有一个 release/extract 操作，但它应该是消费式的。** scpp 不想要
一个"抽走目标以后留下一个空 callable"的哨兵状态，就像它不想要
move 之后留下一个还能继续用、只是内部为空的状态一样。所以可接受的形状，
是 `unique_ptr::release()` 的精神等价物，但用 scpp 已有的 moved-out
状态模型表达：按值把底层 callable 抽出来、且不析构它，而包装器对象自己
此后被当成已消费 / moved-out。

**只有当"擦除"本身就是目的时，才该用这两个包装器。** 如果一个函数只是
想调用别人交给它的闭包或函数对象，[§5.11](#511-泛型函数与-concept) /
[§5.12](#512-闭包lambda-表达式) 那条泛型路径依然更合适：没有堆盒子，
没有擦除后的调度，编译器也能完整看见具体类型。`std::function` 和
`std::move_only_function` 的意义，是补上泛型单态化不方便表达的场景：
把几种不同 callable 类型存在同一个拥有型字段里；按值返回"某个这个
签名的 callable"但不暴露具体类型；或者把一个回调跨 ABI / 架构边界传递，
需要刻意隐藏它的静态类型。

---

[← 上一章：struct 与 class 的语义区分](ch04-struct-vs-class.md) · [目录](README.md) · [下一章：v0.1 支持的子集 →](ch06-safe-subset.md)
