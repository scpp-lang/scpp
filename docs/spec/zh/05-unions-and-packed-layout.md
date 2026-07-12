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

---

[← 上一节：线程安全属性](04-thread-safety-properties.md) · [目录](README.md) · [下一节：常量求值 →](06-constant-evaluation.md)
