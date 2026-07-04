# 7. 编译管线（架构）

```
源码
 └─► 词法分析 (Lexer)
     └─► 递归下降 Parser ──► 统一 AST（safe/unsafe 同一套 AST，带 safety 标记）
         └─► 名称解析 + 类型检查 ──► HIR（去糖：std::move→move hint 等）
             ├─ [unsafe 区] ─────────────────────────────► 直接 lower
             └─ [safe 区] ─► MIR（CFG + 三地址）
                            └─► 借用检查（初始化/move/别名/生命周期）
                                └─► 检查通过后 lower
                 └─► LLVM IR ──► LLVM 优化 ──► 目标二进制
```

要点：
- **AST 统一**：safe 与 unsafe 代码共用一套 AST，节点上带 safety 上下文位。
- **借用检查只在 safe 区的 MIR 上进行**；unsafe 区跳过，直接下降。
- 前端对 unsafe/普通 C++ 只需"够用"——不追求完整 C++ 兼容。
- MIR 显式化：所有权转移、borrow 起止、drop 插入点、CFG。
- 这张图是**单个文件**的管线（现在也是唯一存在的一种）。
  [ch11](ch11-modules-and-libraries.md) 定义了多文件场景下，被
  `import` 的模块的签名/struct 布局如何喂给上面"名称解析 + 类型检查"
  这一步——管线本身不变，只是输入来源从一个文件变成了多个。
- **为什么直接 lower 成 LLVM IR，而不是转成 C++ 文本交给 Clang 编译**：
  后者认真考虑过——专门为了几乎免费拿到跟既有 C++ 库互操作的能力
  （真实的模板、class、异常、RTTI，全部交给真正的 C++ 编译器处理）。
  最后否决了，原因：(a) 这不会让上面 checked 区域这条管线变得更简单
  或更小——不管选哪条路，movecheck 的工作量完全一样，光凭这一点不足
  以justify 把 codegen 整个推翻重来；(b) 对 safe 区代码而言，它的
  优化上限严格**更低**：LLVM 的 `noalias`/`alias.scope` metadata 能
  表达"只在函数体某个子区域内"这种借用检查器凭自己 NLL 精度分析出来
  的别名事实，C++ 源码层面没有任何写法（连 `__restrict` 都不行——它
  最终只能映射到更粗粒度的、整个参数生命周期的 `noalias` **属性**）
  能触达这种更细粒度的 metadata。`extern "C"`
  （[§2.1](ch02-boundary-rules.md)）仍然是 scpp 跟外部世界唯一、故意
  收窄的互操作机制（见 [§8](ch08-open-questions.md) 第 6 条）。

---

[← 上一章：v0.1 支持的 safe 子集](ch06-safe-subset.md) · [目录](README.md) · [下一章：未决问题 →](ch08-open-questions.md)
