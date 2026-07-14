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

此前关于“是否允许未检查的整数到枚举 cast”的开放问题，现已由
[docs/spec/zh/09-enumeration-conversions.md](spec/zh/09-enumeration-conversions.md)
中的规范定案。
