# 9 union 类型与 packed 布局

## 9.1 Union types（union 类型）[class.union.scpp] {#91-union-types}

(1) 一个 *class-key* 为 `union` 的类声明，会声明一个 union 类型。

(2) union 类型是一种聚合类型，它的各个非 static 数据成员共享同一份存储表示；
每个这类成员都从偏移 0 开始。

(3) 本文档目前没有定义任何 tagged-union 构造。因此，对
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) 而言，每个 union 都按
未加标签的 union 处理。

(4) 访问一个 union 的非 static 数据成员，是
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe) 下的一项受管制操作。

(5) 当这种访问按 [§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe)
被允许时，被选中的成员表达式除此之外仍然受该成员类型本来适用的普通规则
约束，包括 [§6](02-ownership-and-move.md) 与
[§7](03-dereference-and-member-access.md)。

【注：unsafe gate 管的是"当前把这些字节按哪一种表示来解释"。它不会暂停
作用在结果表达式上的所有权、生命周期或其它规则。——注释结束】

## 9.2 Packed layout attribute（packed 布局 attribute）[dcl.attr.scpp.packed] {#92-packed-layout-attribute}

(1) *attribute-namespace* `scpp` 里的 *attribute-token* `packed`
（[dcl.attr.grammar]）可以出现在一个 *attribute-specifier-seq* 中，并且附着于
一个 struct 或 union 的声明。不得出现 *attribute-argument-clause*。

(2) 如果一个包含 attribute-token `packed` 的 *attribute-specifier-seq*
附着到 struct 或 union 的声明之外的任何东西上，程序不合法（ill-formed）。

(3) 如果 `[[scpp::packed]]` 附着于一个 struct 声明，那么这个 struct 的各个
非 static 数据成员按声明顺序布局，相邻成员之间不插入 padding，而且该
struct 的对齐要求为 1。

(4) 如果 `[[scpp::packed]]` 附着于一个 union 声明，那么这个 union 的每个
非 static 数据成员偏移都是 0，而且该 union 的对齐要求为 1。

(5) 对一个附着了 `[[scpp::packed]]` 的 union，它的大小是足以容纳其最大
非 static 数据成员的最小大小。

(6) 这个 attribute 是一个可供任何用户声明的 struct 或 union 使用的普通
attribute。引入它是为了表达某种外部所要求的字节布局；这里不存在任何被特殊
对待的库类型名字。

【注：下面这些声明，对应的是那类字节表示被精确规定好的常见外部 ABI 布局：

```cpp
union [[scpp::packed]] epoll_data_t {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
};

struct [[scpp::packed]] epoll_event {
    uint32_t events{};
    epoll_data_t data{};
};
```

ISO C++ 自身没有标准化的 packed-layout attribute；现有 C/C++ 工具链通常用
`__attribute__((packed))` 或 `#pragma pack` 这类扩展来暴露这项能力。——注释结束】

## 9.3 对齐说明符与对齐查询（Alignment specifier and alignment query）[dcl.align] {#93-alignment-specifier-and-query}

(1) 除本小节另有修改外，[dcl.align] 与 [expr.alignof] 原样适用于 SCPP26。

(2) 对齐说明符 `alignas` 以与 ISO C++ 完全相同的语法受支持：

  (2.1) `alignas(constant-expression)`，其中 constant-expression 必须是整型常量表达式；

  (2.2) `alignas(type-id)`，它请求与该 type-id 相同的对齐要求，也就是
  `alignas(alignof(type-id))`；以及

  (2.3) 同一声明上可以出现多个 alignment-specifier；此时取其中最严格、且非 0 的对齐值。

(3) alignment-specifier 只能附着于：

  (3.1) 一个变量声明；

  (3.2) 一个不是 bit-field 的非 static 数据成员声明；或

  (3.3) 一个 *class-key* 为 `struct`、`class` 或 `union` 的类声明。

如果 alignment-specifier 附着到其它任何声明上，程序不合法（ill-formed）。

【注：SCPP26 v1 目前并未在其它地方正式规定 bit-field 声明；不过本小节仍然跟随
ISO C++，明确不允许 `alignas` 附着于 bit-field。——注释结束】

(4) `alignas(0)` 没有效果。除此之外，constant-expression 操作数必须求值为一个
受支持的有效对齐值。在 SCPP26 v1 中，这意味着一个被目标 ABI 接受的正的 2 的幂。

(5) 如果一个 alignment-specifier 请求的对齐要求比该声明实体或类类型的自然对齐
更弱，则程序不合法（ill-formed）。

(6) 如果 `alignas` 附着于一个类声明，它会改变该类类型自身的对齐要求。此后，
该类型的完整对象、子对象以及数组的布局都必须满足这一更严格的对齐要求。

(7) 如果 `alignas` 附着于一个变量或非 static 数据成员，它会改变该被声明对象或
子对象的最小对齐要求；但它本身并不会改变该声明所命名的底层类型的对齐要求。

【注：例如，`alignas(16) int a[4];` 要求对象 `a` 按 16 字节对齐，但它并不会改
变数组类型 `int[4]` 自身的对齐要求。——注释结束】

(8) `alignof(type-id)` 查询也按 ISO C++ 的相同含义受支持：它会产出该类型的对齐
要求，并且结果是一个 `std::size_t` 类型的整型常量表达式。

(9) SCPP26 中的 `alignof` 采用 ISO C++ 的 `type-id` 形式。GNU 风格的
`alignof(expression)` 扩展不属于 SCPP26。

(10) `alignof(type-id)` 可以出现在任何要求整型常量表达式的地方，包括 `alignas`
的操作数内部。

(11) 如果 `[[scpp::packed]]` 附着于一个 struct 或 union 声明：

  (11.1) 则不得再有任何 alignment-specifier 附着于该同一声明，或附着于它的
  任一非 static 数据成员；并且

  (11.2) 该 struct 或 union 的任一非 static 数据成员，也不得具有这样一种类类型
  （或该类类型的数组类型）：该类类型的对齐要求因为附着于该类类型声明的
  alignment-specifier 而被提高。

违反此规则的程序不合法（ill-formed）。

【注：`[[scpp::packed]]` 的存在目的，是要求一个精确的 packed 外部字节布局：
整体对齐为 1，且不插入隐式 padding。若再与 `alignas` 组合，就会产生互相矛盾的
布局要求。尤其是：如果某个成员子对象所属的类类型本身因为 `alignas` 而要求
over-aligned 放置，那么 packed 对象就无法再对它作出正确放置保证。——注释结束】

(12) 例子：

接受：

```cpp
struct alignas(32) block {
    std::uint64_t words[4];
};

alignas(block) unsigned char scratch[sizeof(block)];

struct header {
    std::uint16_t len;
    alignas(8) std::uint32_t checksum;
};
```

拒绝：

```cpp
alignas(3) int x;                    // 不是 2 的幂对齐
struct alignas(1) big { long long x; }; // 比该类型的自然对齐更弱
struct [[scpp::packed]] alignas(8) p { int x; }; // packed 与 alignas 冲突
struct alignas(8) block8 { int x; };
struct [[scpp::packed]] bad { char tag; block8 payload; }; // packed 不能包含 over-aligned 类类型成员
```

## 9.4 数组声明符（Array declarators）[dcl.array] {#94-array-declarators}

(1) 除本小节另有修改外，[dcl.array] 原样适用于 SCPP26。本小节统一适用于
任何出现数组声明符的场合：局部变量声明、非 static 数据成员声明、函数形参
声明，或者 [dcl.array] 本身允许出现数组声明符的任何其它场合。

(2) 给出数组边界的 constant-expression，在语法上与 ISO C++ 中完全相同，
是一个普通的表达式；除 (4)-(7) 之外，本文档不对它的形式施加任何限制。

【注：特别地，本文档并不把这个表达式限制为单个整数字面量 token。ISO
C++（因而 SCPP26）在这个位置所接受的形式，包括但不限于：一个 `sizeof`
表达式（[expr.sizeof]）、一个 `alignof(type-id)` 查询
（[§9.3](#93-alignment-specifier-and-query)）、一个指代 `constexpr` 变量的
*id-expression*（[dcl.constexpr]），以及由这些子表达式构成的、可以嵌套
任意深度的算术或比较组合。——注释结束】

(3) 对数组边界 constant-expression 的求值，是 required constant
evaluation（[§7](06-constant-evaluation.md)）；该条款规定这次求值所允许
的操作数与操作。

【注：[§7.2](06-constant-evaluation.md#72-支持的子集supported-subsetexprconstscppsupport)
保证支持 `sizeof`/`alignof` 查询，以及对整数操作数的算术与比较运算等操作。——注释结束】

(4) 数组边界 constant-expression 必须是一个类型为 `std::size_t` 的转换后
的常量表达式（[expr.const.const]）；它的值给出该数组的边界，也就是数组中
的元素个数，而且这个值必须大于零。这两项要求中有任一项不满足，程序就不
合法（ill-formed）。

(5) 如果数组边界 constant-expression 不是一个常量表达式，程序不合法
（ill-formed）。

【注：一个非 `const`、非 `constexpr` 的局部变量，或者任何其它直到运行期
才能确定其值的实体，都不能用在常量表达式中（[expr.const]），因此也不能被
数组边界读取。本文档并不会因此引入 variable-length array：与某些 C++
实现把 VLA 作为不合规扩展加以接受不同，一个不是常量表达式的数组边界
constant-expression，在 SCPP26 中仍然不合法（ill-formed），这与严格的
ISO C++ 完全一致。——注释结束】

(6) 如果对某个数组边界 constant-expression 的 required constant
evaluation 需要用到某个 class 类型的大小、对齐要求或者完整性，而该 class
类型在程序的那个位置尚未完整——包括这个数组成员所属声明本身所在的那个
class 类型——那么程序不合法（ill-formed）。

【注：这与 ISO C++ 已经施加给 `sizeof` 的规则相同（[expr.sizeof]）：一个
class 类型在其自身定义的右花括号 `}` 之前都是不完整的，因此一个非 static
数据成员的数组边界不能对它自身所属的这个 class 使用 `sizeof`。——注释
结束】

(7) 如果一个数组边界 constant-expression 依赖于某个模板参数，也就是说它是
**value-dependent（值依赖）**的（[temp.dep.constexpr]），那么对这个数组
边界 constant-expression 的 required constant evaluation，在声明该模板
参数的模板的每次实例化（[temp.point]）时执行，使用该次实例化中与该模板
参数对应的模板实参。求值得到的数组边界 constant-expression 按 (2)-(6)
处理，与任何其它数组边界 constant-expression 完全一样。

【注：例如，在 `template<typename T> struct Box { char storage[sizeof(T)];
};` 里，数组边界 `sizeof(T)` 在模板定义内部是 value-dependent 的。实例化
`Box<int>` 会用 `int` 代换 `T`，因此这次实例化的数组边界就是普通的常量
表达式 `sizeof(int)`；实例化 `Box<double>` 同样会为这次实例化产生数组
边界 `sizeof(double)`。这与 ISO C++ 自身对 value-dependent 表达式的处理
方式一致（[temp.dep.constexpr]、[temp.point]）：这个表达式在模板定义里
只确定一次，但会在每次实例化时分别求值，使用该次实例化的模板实参。——
注释结束】

(8) 例子：

接受：

```cpp
char literal_bound[8];

char sizeof_bound[sizeof(int) + 4];

constexpr int kBufferSize = 64;
char named_constant_bound[kBufferSize];

struct Header {
    std::uint16_t a;
    std::uint32_t b;
};
char alignof_bound[alignof(Header) * 2];

template<typename T>
struct Box {
    char storage[sizeof(T)]; // 在模板定义里是 value-dependent 的；在每次
                              // 实例化（(7)）时被解析为 sizeof(int)、
                              // sizeof(double) 等
};
```

拒绝：

```cpp
int n = 5;
char runtime_bound[n];      // 不合法：n 不是常量表达式（这不是 VLA）

struct Self {
    char buf[sizeof(Self)]; // 不合法：Self 在这里还不完整
};

char zero_bound[0];         // 不合法：数组边界必须大于零
char negative_bound[-1];    // 不合法：数组边界必须大于零
```

---

[← 上一节：线程安全属性](04-thread-safety-properties.md) · [目录](README.md) · [下一节：常量求值 →](06-constant-evaluation.md)
