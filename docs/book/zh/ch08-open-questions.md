# 8. 未决问题（Open Questions，需后续拍板）

1. **下标越界**：safe 区 `vector[i]` / `span[i]` 是插入运行时边界检查
   （像 Rust），还是要求用带检查的 API？**已定并实现（M6）**：`span[i]`
   默认插入运行时边界检查，越界调用 `abort()`（`vector` 还没实现，但
   会沿用同一策略）——这是 `safe` 代码的行为。在 `unsafe { }` 里（或者
   整个 native 函数里），这条检查会被跳过——待遇、理由都跟整数溢出检查
   一样（见下面 Q2 / [§5.8](ch05-static-checks.md)）：跳过一个 scpp 自己
   加的**运行时**检查，不会有"损坏的记账泄漏到外面 safe 代码"这种风险，
   而这正是 move/借用/生命周期检查必须无条件继续跑的理由（见
   [§1.3](ch01-safety-context.md)）。
2. **整数溢出**：safe 区是否检查有符号溢出？**已定**：查——`safe` 代码里
   有符号无符号都查，溢出就 `abort()`，无条件生效（不分 debug/release）；
   在 `unsafe { }` 或者 unsafe 函数里不检查，但保证 wrap（绝不是 UB），
   靠 scpp 自己的算术 codegen 从不打 LLVM 的 `nsw`/`nuw` 做到。除以 0/
   模以 0/`INT_MIN / -1` 无条件 `abort()`，`safe`/`unsafe` 都一样——
   硬件本身就没有 wrap 后的结果可退回去用。这里故意跟 Rust 的
   debug-only 默认不一样（完整理由见 [§5.8](ch05-static-checks.md)，
   包括为什么溢出检查——跟 [§5.1-§5.4](ch05-static-checks.md) 那批检查
   不一样——能安全地加入 `unsafe { }` 放宽的范围，而不会有
   [§1.3](ch01-safety-context.md) 那条"泄漏到外面 safe 代码"规则想防的
   风险）。
3. **panic 模型**：越界/断言失败如何终止？`std::terminate` 还是自定义
   panic + 栈展开？**已定并实现（M6）**：直接调用 libc 的 `abort()`（比
   `std::terminate()` 更底层、不依赖 C++ 运行时的 terminate-handler 机制，
   效果一致——进程立即终止，不做栈展开）。
4. **内部可变性**：是否引入等价 `Cell`/`RefCell` 的机制承载合法可变别名？
   **已定，只做第一阶段**：复用真实 C++ 的 `mutable` 关键字，但更严格——
   `mutable` 字段必须是平凡类型（套用 `struct` 自己那条字段平凡性规则，
   见 [§4.1](ch04-struct-vs-class.md)），不管 `this` 是不是 `const` 都
   能读写，但**永远不能**被引用或者取地址（见
   [§4.2](ch04-struct-vs-class.md)/[§5.9](ch05-static-checks.md)）。这
   白送了 scpp `Cell<T>` 那一半（零运行时开销，因为一个永远没法被引用
   的值不可能产生别名），而且用的是现成的 C++ 语法。`RefCell` 那一半
   （借用非平凡内部状态的真正引用，运行时借用计数器，违规就 panic/
   abort）真实 C++ 没有现成名字可以借用，需要真正的新机制——留到以后
   单独一轮再定，不是这次决定的一部分。
5. **`safe` 与 `const` 的关系**：`const` 成员函数在 safe 区如何映射借用？
   **已定**：`this` 被当成一个隐式引用形参——`const` 方法里是 `const T&`，
   否则是 `T&`——套用跟其它引用完全一样的别名 XOR 可变、整体保守字段
   访问、生命周期省略规则（见 [§5.9](ch05-static-checks.md)）。同一轮
   一起定的相关内容：`class` 的成员**变量**（包括类级别常量）永远不能
   是 `public`，只有成员**函数**可以——外部代码永远走方法调用，不能直接
   碰字段（见 [§4.2](ch04-struct-vs-class.md)）；类级别常量通过一个
   `static consteval` 函数暴露，不是公开数据成员（scpp 为什么没有
   `constexpr` 修饰的函数，见 [§6](ch06-safe-subset.md)）。继承（以及
   `protected`）依然搁置，不属于这一轮。
6. **ABI / 与现有 C++ 库互操作**：safe 代码调用第三方头文件（全是 unsafe）
   的工程化方式（是否全部视为 `unsafe`）。**已定**：`extern "C"`
   （[§2.1](ch02-boundary-rules.md)，设计已定稿）是 scpp 跟外部世界
   **唯一**的互操作机制；scpp 代码跨文件互相共享由
   [ch11](ch11-modules-and-libraries.md) 回答（设计已定稿）。跟**既有
   的、原样不改的 C++ 库**互操作这件事本身（任意 class、模板、重载、
   异常、RTTI）明确**不追求**——考虑过"把检查通过的 scpp 转成真实
   C++ 文本、交给 Clang 编译"这条路（能让这件事变简单，但代价是要
   把已经跑通的、直接产 LLVM IR 的 codegen 整个推翻重来），最后否决
   了：对 safe 区代码而言，自己直接产 LLVM IR 的优化上限严格更高
   （借用检查器凭自己 NLL 精度证明出来的别名事实，能对应到 LLVM 的
   scoped `alias.scope`/`noalias` metadata，这个精细度在 C++ 源码
   语法层面没有对应写法——连 `__restrict` 都够不到，因为 `__restrict`
   最终只能映射到更粗粒度的、整个参数生命周期的 `noalias` 属性）。
7. **语言/编译器命名、文件扩展名**。
8. **可恢复错误怎么处理**：走异常，还是走值类型？**已定**：scpp 哪里都
   没有异常（没有 `throw`/`try`/`catch`）。所有失败要么是**bug**（abort——
   已经由上面的 Q3 定了），要么是**可恢复的、预期内的情况**（一个普通的
   `std::expected<T, E>` 值，由编译器强制检查——见
   [§5.6](ch05-static-checks.md)）。构造函数/析构函数遵循同样的划分（见
   [§4.2](ch04-struct-vs-class.md)）：可以在前置条件不满足时 abort，但
   不能产生可恢复错误——可失败的构造改走一个返回 `std::expected<T, E>`
   的 `static` 工厂函数（经典 C++ "named constructor idiom"）。**传播**
   一个 `std::expected` 的错误到调用者，v0.1 用普通 `if`/`else`——曾经
   考虑过一个类似 Rust `?` 的后缀运算符（拼成 `??`，因为 C++ 已经用裸
   `?` 表示三元运算符了），最后**否决**了：跟 scpp 其它所有语法不一样，
   一个全新的运算符 token 没法被真正的 C++ 编译器忽略或者擦除掉，这会
   永久打破"把 `safe`/`unsafe` 从 scpp 文件里去掉，剩下的就是能被真 C++
   编译器原样接受的普通文件"这条性质（见 [ch00](ch00-design-philosophy.md)
   §2）——真编译器解析到第二个 `?` 就会硬报错（trigraph 是唯一曾经赋予
   `??` 含义的东西，C++17 就删掉了）。要不要重新考虑，等 C++ 标准自己在
   这块进一步演进之后再看。
9. **`const T*` 和 `T*` 是同一个类型吗？** [§5.7](ch05-static-checks.md)
   （`&expr` 设计）早先的草稿曾经假设 scpp 的 `const T*`/`T*` 统一成了
   一个不追踪的类型——这是错的，不管在真实 C++ 还是 scpp 里都不是，是在
   讨论中被发现并纠正的。真实 C++ 一直把它们当成两个不同的类型（单向
   隐式 `T* -> const T*` 转换，反过来要 `const_cast`），Rust 的
   `*const T`/`*mut T` 在编译期强制同样的划分——哪怕在 `unsafe` 里，通过
   `*const T` 写也会被拒绝。**已定**：scpp 认真追踪这个区分（新增一个
   `is_mutable_pointee` 标记，照抄 `is_mutable_ref` 区分 `T&`/`const T&`
   的方式）；单向隐式转换是真实 C++ 本来就有的规则，不是新发明的；通过
   `const T*` 写是普通的、无条件强制的类型错误，不是 `unsafe { }` 会
   放宽的东西。v0.1 没有 `const_cast` 等价物（见
   [§5.7](ch05-static-checks.md)）。
10. **Namespace 设计**：scpp 支持 C++ namespace 特性到什么程度，跟
    module 怎么互动？**已定**：scpp 原样复用真实 C++ 的 namespace 语法
    （包括 C++17 单行嵌套写法），带三条永久限制——任何地方都不允许
    `using namespace`（只允许单名的 `using foo::bar;`）；不支持匿名
    namespace（跟 module 的导出面机制重复，见
    [§11.3](ch11-modules-and-libraries.md)）；完全没有 ADL（参数依赖
    查找），永远没有——调用永远只从词法作用域和显式的 `using` 声明解析，
    不看参数类型（见 [§11.4](ch11-modules-and-libraries.md)）。有一条
    真实 C++ 里没有对应的新规则，把 namespace 和 module 在导出这条边界
    上绑在了一起：一个标了 `export` 的声明，只有词法上落在一个跟当前
    module 自己点分名字匹配（作为前缀，允许更深嵌套）的 namespace 里，
    才算真的导出了（见 [§11.5](ch11-modules-and-libraries.md)）——刻意
    不引入隐式/默认 namespace（早期草稿考虑过，因为没法在 erasure 下
    存活而被否决，见 [ch00](ch00-design-philosophy.md) §2/§6）。这把
    真实 C++ "这个限定名到底哪个头文件定义的"这种猜测，变成了一个机械
    保证的事实：任何一个完整限定名都能唯一确定该 `import` 哪个 module。
    跨多个 import 的 module 做限定名解析，用的是对照实际被 import 的
    module 名字集合做最长前缀匹配；如果两个被 import 的 module 恰好都
    能解析同一个限定名，是编译错误（"限定名有歧义"），不是静默按最长
    匹配挑一个，理由跟否决 ADL 一样：一个不相关的、后来才加的
    `import`，永远不该悄悄改变已有代码的含义。
11. **函数重载**：scpp 允不允许多个函数共用一个名字，候选怎么决议？
    **已定**：允许，只按参数列表区分，永远不靠返回类型（见
    [§5.10](ch05-static-checks.md)）。真实 C++ 自己的重载决议是靠给
    隐式转换序列排等级（精确匹配 > 提升 > 转换 > ……）——设计这一节的
    时候拿真实编译器验证过，实际用起来比看着复杂得多：提升只会精确
    指向 `int`/`unsigned int`/`double`，不是"离得最近的更宽类型"（所以
    `int8_t` 在 `int16_t`/`int32_t`/`int64_t` 几个重载之间竞争时，赢的
    是哪个恰好是这台机器的 `int`，不是离得最近的那个），而两个都只是
    "普通转换"级别的候选（比如 `int16_t` 和 `int64_t` 竞争一个
    `int32_t` 实参）直接判歧义，压根没有"更近的赢"这条规则。考虑过
    照抄 Java/C# 的另一条路（它们的变宽转换链会选需要变宽步数最少的
    候选，是个真实存在、但跟 C++ 不兼容的规则），最后**决定改学
    Rust/Swift/Kotlin**：scpp 任何两个不同的标量类型之间都没有隐式
    转换，把 `bool`/`char` 现有的规则推广到整个数值家族（见
    [§6](ch06-safe-subset.md)）——任何转换都要显式 cast，没有例外。
    这样重载决议就简化成了纯粹的类型精确匹配，而（因为两个重载不可能
    声明成完全相同的参数类型列表）这一步本身永远不会产生歧义结果：
    最终结果只有"恰好找到一个匹配"或者"一个都没匹配上"（要求显式
    cast）。按值/按引用（`f(T)`/`f(T&)`/`f(const T&)`）是另一条、正交
    的轴，靠现有的"必须显式 `std::move`"规则
    （[§5.1](ch05-static-checks.md)）白送消歧义；一个可变左值同时能
    匹配 `T&` 和 `const T&` 时，`T&` 赢，照抄真实 C++——这正是让
    const/非 const 方法重载（`get()`/`get() const`）能工作起来的原因，
    填上了 [§5.9](ch05-static-checks.md) 里标记过的坑。作用域规则、
    `using` 声明的导入方式、以及刻意不做 ADL，全部照抄现有规则不变
    （完整设计见 [§5.10](ch05-static-checks.md)）。

---

[← 上一章：编译管线](ch07-compilation-pipeline.md) · [目录](README.md) · [下一章：MVP 里程碑 →](ch09-milestones.md)
