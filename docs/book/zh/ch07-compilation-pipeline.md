# 编译管线（架构）

```
源码
 └─► 词法分析 (Lexer)
     └─► 递归下降 Parser ──► 统一 AST（节点上带一个 unsafe 块标记）
         └─► 名称解析 + 类型检查 ──► HIR（去糖：std::move→move hint 等）
             └─► MIR（CFG + 三地址）
                 └─► 借用检查（初始化/move/别名/生命周期——无条件运行；
                     unsafe 嵌套计数器只在这一趟检查内部放宽 §5.5
                     那几个具体操作，从不跳过这一趟检查本身）
                     └─► 检查通过后 lower
                          └─► LLVM IR ──► LLVM 优化 ──► 目标二进制
```

要点：
- **AST 统一**：每个函数共用同一套 AST 形状，节点上带一个 unsafe 块
  标记，标出哪些语句词法上位于 `[[scpp::unsafe]] { }` 复合语句内部
  （[§1](ch01-safety-context.md)）；这个标记是在解析该语句原本就存在的
  attribute-specifier-seq 时设上的，不是靠一个单独的 `unsafe` 语法产生式。
  如果一个函数自己的声明也带同样的 attribute，函数体内每一条语句都会
  直接被设上这个标记（见 [§1.2](ch01-safety-context.md)），而且函数自己
  的 `FunctionDecl` 节点上还会带一个独立的标记，供检查调用点的那趟处理
  用来给它自己的调用点把关，跟现有 `extern "C"` 检查的做法一样。
- **借用检查在每个函数的 MIR 上无条件运行**——`[[scpp::unsafe]] { }` 从不跳过
  这一趟检查；它只在检查内部放宽 [§5.5](ch05-static-checks.md) 里那份
  固定、列举出来的操作（裸指针解引用、调用 `extern "C"` 函数等等），
  跟 Rust 自己的借用检查器在 `unsafe fn`/`unsafe { }` 块内部照样继续跑
  是同一个道理。
- 前端只需要处理 [§6](ch06-safe-subset.md) 支持的子集里的那些
  构造——对任意的、不支持的构造，不追求完整 C++ 标准兼容。
- MIR 显式化：所有权转移、borrow 起止、drop 插入点、CFG。
- 这张图是**单个文件**的管线。
  [ch11](ch11-modules-and-libraries.md) 定义了多文件场景下，被
  `import` 的模块的签名/struct 布局如何喂给上面"名称解析 + 类型检查"
  这一步——管线本身不变，只是输入来源从一个文件变成了多个。
- **为什么直接 lower 成 LLVM IR，而不是转成 C++ 文本交给 Clang 编译**：
  后者认真考虑过——专门为了几乎免费拿到跟既有 C++ 库互操作的能力
  （真实的模板、class、异常、RTTI，全部交给真正的 C++ 编译器处理）。
  最后否决了，原因：(a) 这不会让上面这条受检查的管线变得更简单
  或更小——不管选哪条路，借用检查的工作量完全一样，光凭这一点不足
  以justify 把 codegen 整个推翻重来；(b) 它的
  优化上限严格**更低**：LLVM 的 `noalias`/`alias.scope` metadata 能
  表达"只在函数体某个子区域内"这种借用检查器凭自己 NLL 精度分析出来
  的别名事实，C++ 源码层面没有任何写法（连 `__restrict` 都不行——它
  最终只能映射到更粗粒度的、整个参数生命周期的 `noalias` **属性**）
  能触达这种更细粒度的 metadata。`extern "C"`
  （[§2.1](ch02-boundary-rules.md)）仍然是 scpp 跟外部世界唯一、故意
  收窄的互操作机制（见 [§8](ch08-open-questions.md) 第 6 条）。

---

[← 上一章：v0.1 支持的子集](ch06-safe-subset.md) · [目录](README.md) · [下一章：未决问题 →](ch08-open-questions.md)
