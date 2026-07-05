# 4. struct 与 class 的语义区分（内存布局 / ABI 固定）

这是与 C++ 的一个关键差异：C++ 里 `struct` 和 `class` 几乎等价（仅默认可见性
不同）；scpp 里两者语义完全不同。

## 4.1 struct：纯平凡数据（trivial aggregate）

- `struct` 只能包含**平凡（trivial）类型**的成员，递归定义如下：
  - 标量类型：`bool`、整型、浮点、`char`。
  - 裸指针 `T*`（不带编译器跟踪的生命周期；解引用仍需 `unsafe {}`，见
    [§2](ch02-boundary-rules.md)）。
  - 其他同样满足本规则的 `struct` 类型（递归）。
  - 平凡类型的定长数组。
- 以下类型**禁止**作为 `struct` 成员，必须改用 `class`：
  - 引用 `T&` / `const T&`。
  - `std::span<T>`、`std::string_view`（带生命周期检查的借用视图）。
  - `std::unique_ptr<T>`、`std::shared_ptr<T>`、`std::vector<T>`、
    `std::string`，或任何参与所有权/借用检查的类型。
- 未通过此规则的 `struct` 定义是编译错误（提示"改用 class"），而不是静默
  降级。

**初始化**：`struct` 局部变量/成员若未显式初始化，编译器保证整个对象**按
位清零**（zero-init）。标量成员等价于 `0` / `false` / `0.0`；裸指针成员
等价于 `nullptr`。这不是 `struct` 的特例，而是 scpp**所有类型**共享的通用
规则——见 [§5.4](ch05-static-checks.md) 的完整说明。

**Copy 语义**：`struct` 值可以自由、隐式地按位复制，不参与
[§5](ch05-static-checks.md) 描述的 move/借用检查——因为它不携带任何
生命周期或独占所有权语义。这不是一个"可选的 Copy trait"（对比 Rust 需要
显式 `#[derive(Copy)]`）：一个类型只要声明为 `struct`，编译器就会验证并
保证它满足平凡性，`struct` 关键字本身就是显式声明。

## 4.2 class：拥有资源/参与检查的类型

- `class` 可以包含任意类型的成员，包括 `unique_ptr`、`vector`、`span`、
  其他 `class`，或本身携带生命周期/所有权语义的字段。
- `class` 参与 [§5](ch05-static-checks.md) 描述的所有权/移动/借用/
  生命周期检查；**不**保证 zero-init（需要显式构造）。
- v0.1 阶段用户自定义 `class` 类型的完整检查规则（构造/析构、方法体内部
  借用等）超出范围，见 [§8](ch08-open-questions.md) 的 backlog；v0.1
  首先只对标准库提供的 `unique_ptr` 等类型做检查（M2 里程碑）。
- **可失败的构造与析构**：
  构造函数/析构函数没有通道把 `std::expected<T, E>` 传回去——scpp 根本
  没有异常可以替代地抛出（见 [§5.6](ch05-static-checks.md)/
  [§8](ch08-open-questions.md)），这两个特殊成员函数也都不返回普通值。
  这不是一条需要额外强制的新规则，而是"scpp 哪里都没有异常"这个事实
  白送的推论。
  - 构造函数/析构函数仍然可以校验前置条件，但校验失败只能靠 abort 处理
    （归为 [§8](ch08-open-questions.md) Q3 意义上的"bug"）——不能产生
    可恢复错误。
  - 如果一个类型的构造确实可能因为可恢复的原因失败（文件不存在、输入
    非法……），就不应该通过构造函数暴露这种失败，而是改用一个普通的
    `static` 成员函数，返回 `std::expected<T, E>`，只有确定成功之后才
    调用那个"保证成功"的构造函数。推荐做法（只需要普通 C++ 的访问控制
    就能落实，不需要新机制）：把那个朴素构造函数设成 `private`，让外部
    只能走这个工厂函数——经典的 C++"named constructor idiom"
    （Marshall Cline 的 C++ FAQ），不需要任何新增的 scpp 语法。
- **访问控制**：成员**变量**
  ——包括类级别的常量——永远不能是 `public`，只有成员**函数**才可以。
  在成员变量上面写 `public:` 是编译错误。这样一来外部代码永远只能通过
  方法调用去碰一个类的数据，不能直接摸字段——这也让
  [§5.9](ch05-static-checks.md) 里"方法借用检查"的设计变得可控：借用
  检查器只需要理解**方法调用**这一种跨越类边界的方式，不需要再处理
  "外部随意结构性访问字段"这种情况（`struct` 目前就是这么处理的）。
  - 类级别的常量通过一个 `static consteval` 函数暴露，而不是一个公开的
    数据成员（scpp 为什么没有 `constexpr` 修饰的函数，见
    [§6](ch06-safe-subset.md)）——这样跟直接暴露一个公开常量相比，
    运行时开销完全一样（都是零）。
- **v0.1 没有继承**（暂时搁置，不是永久排除——见
  [§8](ch08-open-questions.md)）：`protected` 因此也不是一个被识别的
  访问修饰符，因为它只有相对于派生类才有意义。以后如果真要设计继承，
  这两个一起重新考虑。编译期多态（通过一套共享的、受检查的接口去调用
  形状不同的类型）不需要等继承——见
  [§5.11](ch05-static-checks.md) 里基于 `concept`/`requires` 的泛型
  函数。
- **内部可变性，第一阶段**（回答了
  [§8](ch08-open-questions.md) Q4 里 `Cell` 那一半，`RefCell` 那一半
  搁置）：scpp 复用真实 C++ 的 `mutable` 关键字，但语义比真实 C++
  **严格得多**（真实 C++ 里 `mutable` 字段完全不设防）。一个 `mutable`
  成员变量：
  - 必须是**平凡（trivial）类型**，套用 §4.1 已经给 `struct` 字段定好的
    同一条规则（标量、裸指针、其它平凡类型、这些类型的定长数组）——
    对应 Rust `Cell<T>` 要求 `T: Copy` 的那条限制；
  - 可以通过**任何** `this` 读写，不管是不是 `const`——`const` 不再
    拦这一个字段的直接访问；
  - **永远不能**被引用或者取地址（`T&`/`const T&` 绑定、`&expr`）——
    两种都是编译错误，无条件成立，不管在不在 `unsafe { }` 里都一样。
  正因为永远不可能有引用指向它，运行时压根不需要检查任何别名风险——
  读写就编译成普通的内存读写指令，跟 Rust 的 `Cell::get`/`Cell::set`
  一样便宜。这能覆盖常见场景（计数器、标志位、小的缓存值）；`RefCell`
  那种场景（借用非平凡内部状态的真正引用，运行时计数检查，违规就
  panic/abort）真实 C++ 里没有现成名字可以借用，需要真正的新机制
  （运行时借用计数器、RAII guard 类型）——留到以后单独一轮再定。
  `mutable` 在 `struct` 上没有意义（`struct` 压根没有 `this`/方法/
  const 访问控制这些概念），也不接受这个写法。

## 4.3 内存布局与 ABI（固定，不作为 impl-defined 留白）

scpp 把 `struct` 的内存布局**钉死**为目标平台的 **Clang ABI**：在相同
target triple 下，scpp 编译器产出的 `struct` 布局必须与 Clang 编译等价
C 结构体产生的布局逐字节一致。具体规则：

1. 成员按源码声明顺序排列，编译器**不做重排**。
2. 每个成员按其类型在目标 Clang ABI 下的对齐要求对齐；相邻成员之间按需
   插入 padding。
3. 结构体自身对齐 = 所有成员对齐要求的最大值；总大小向上取整为该对齐的
   倍数（保证数组场景每个元素都正确对齐）。
4. 第一个成员偏移量固定为 0（结构体地址即第一个成员地址）。

实现上：编译器生成非 packed 的 LLVM 具名 struct 类型，交给目标 target 的
`DataLayout` 计算布局——该 `DataLayout` 与 Clang 针对同一 target triple
使用的完全一致，因此自动获得与 Clang/C 兼容的布局，无需 scpp 自行定义
对齐算法。

**v0.1 明确不支持**：
- 位域（bit-field）——各平台/编译器版本实现差异大，暂不纳入。
- 压缩布局（等价于 `#pragma pack(1)` / `__attribute__((packed))`）——留待
  后续版本通过显式属性（如 `alignas` / `packed`）支持，映射到 LLVM 的
  packed struct 类型。

---

[← 上一章：语法糖](ch03-syntactic-sugar.md) · [目录](README.md) · [下一章：静态检查 →](ch05-static-checks.md)
