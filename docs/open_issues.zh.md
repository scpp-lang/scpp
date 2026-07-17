# 开放设计问题

本文件记录的是：在实际工作中已经遇到、但被有意留待以后再仔细讨论的语言与设计问题。

它**不是**“某个功能暂时还没实现”的跟踪位置。那是 `docs/missings.md`
的职责。本文件只记录这类情况：实现方向本身被一个尚未定案的设计决策卡住了。

## 线程安全的结构化推导忽略继承而来的 base subobject

当前
[docs/spec/zh/04-thread-safety-properties.md](spec/zh/04-thread-safety-properties.md)
中的线程安全结构化推导规则，在判断一个 class 的 `thread-movable` 与
`thread-shareable` 属性时，只查看该 class 自己声明的非 static 数据成员，
还没有把继承而来的普通 base-class subobject 计入其中。

因此，一个派生的普通 class 目前有可能在它某个继承而来的普通 base-class
subobject 本应阻止这一结果时，仍然被判定为 `thread-shareable` 或
`thread-movable`。这是线程安全通用规则里一个早已存在的缺口，不是接口专属
问题；[docs/spec/zh/11-inheritance-and-interfaces.md](spec/zh/11-inheritance-and-interfaces.md)
里的接口 base 不受这个问题影响，因为它们不贡献任何非 static 数据成员。

后续工作应当扩展结构化推导规则：在确定一个派生 class 自身的线程安全属性时，
把继承而来的普通 base-class subobject 也纳入计算。

## file-scope 全局变量目前仍带着 C++ 跨文件动态初始化顺序风险

PR [#250](https://github.com/scpp-lang/scpp/pull/250) 有意保留了现在已经可用的
file-scope / namespace-scope 变量声明能力，而不是把它回退掉；原因是像
`constexpr int X = ...;` 这样的全局常量确实是一个真实、常见、且需要支持的
用例，而 `alignas` 也可能需要作用在这类全局变量上。可是，这也把经典 C++ 的
“static initialization order fiasco（静态初始化顺序灾难）”重新带了回来：
对于那些需要真正动态初始化的全局变量，这个风险目前还没有被 scpp 限制或诊断。

在同一个 translation unit / 源文件内部，C++ 对动态初始化顺序给出声明顺序保证；
但在不同 translation unit / 源文件之间，动态初始化的相对顺序是未指定的。也就是
说，如果某个文件里的全局变量，其构造过程或运行期初始化依赖另一个文件里的某个
“也需要动态初始化”的全局变量，那么它有可能在对方初始化之前运行，也有可能在之
后运行。

没有 initializer 的全局变量，或者拥有真正常量表达式 initializer 的全局变量
（也就是初始化可以在不依赖跨文件运行期顺序的前提下确定完成），不受这个问题影
响；而这也正是目前可以继续不受限制保留的主要目标用例。

相反，那些需要非常量、运行期计算式动态初始化的全局变量，如果跨文件依赖其他同样
需要动态初始化的全局变量，就可能触发这个风险。scpp 现在还不会限制或诊断这种模
式。目前这种“有意暂时允许、但尚未完全收紧”的行为主要由
PR [#250](https://github.com/scpp-lang/scpp/pull/250) 引入；截至本文写作时，
还没有单独一个已经 open 或 merged 的
`test-agent/global-vars-alignas-coverage` blackbox 覆盖 PR 可在这里交叉引用。

后续工作应当正面处理这个问题。由于 scpp 是 whole-program AOT compiler，而不是
传统 C++ 那种 separate compilation + linking 模型，理论上它也许能在编译期静态检
测并诊断这类跨文件动态初始化顺序依赖；这将不同于“直接一刀切禁止所有 file-scope
全局变量”，也不同于“原样继承 C++ 全部不安全 / 未指定行为并保持静默”。

此前关于“是否允许未检查的整数到枚举 cast”的开放问题，现已由
[docs/spec/zh/09-enumeration-conversions.md](spec/zh/09-enumeration-conversions.md)
中的规范定案。
