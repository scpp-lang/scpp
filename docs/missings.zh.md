# scpp 当前仍需跟踪的能力缺口

此文件是内部积压清单，不是面向读者的书籍内容。

它记录的是相对于 scpp 自身目标中的语言 / 标准库 / 工具体系还缺什么，
而不是相对于某一个参考语言“少了什么”。现在的判断准则很明确：如果普通的 C++
编译器（clang/gcc/msvc）接受这段一模一样的语法，那么 scpp 就可以把对应能力做
成库设施和 / 或语义层；如果那不是合法 C++ 语法，scpp 就不应发明这种新语法。

下面的条目已经按当前 `main` 重新审计；已经修复的项目应当直接移除，而不是作
为历史噪音继续留在列表里。

## 语言/语法缺口（当前重点）

这一节里的项目需要真正的编译器工作：parser、semantic analysis、borrow/type
规则、coroutine machinery、codegen，或者 preprocessor 支持。

- `std::span` 在语言层面的借用 / 绑定规则上仍有缺口：核心的“从定长局部数组绑
  定”已经可以工作，但直接从字符串字面量绑定仍然会失败，而且 `std::span` 在构
  造后仍然不能重新绑定。
- 泛型（模板）`union` 声明会被直接拒绝，尽管带模板的 union 本身是普通、
  合法的 ISO C++ 语法，而且非泛型 union 已经可以工作：union 的成员类型
  永远无法用外层模板自己的类型参数来参数化。
- 多文件的 module 构建目前还不能端到端工作：基于 manifest 的项目构建
  （`scpp build`）会把任何被归类为 module 实现单元的源文件（以裸
  `module name;` 开头、没有 `export` 的文件）直接拒绝，报错 "module
  implementation units are not implemented in project builds yet"，所
  以在真正的构建里，一个 module 的接口和实现现在还无法真正拆分到多个文
  件里。为了支持这种拆分而存在的裸 `extern` 声明形式（一个已导出、无函
  数体的 `extern int f(...);`，其函数体本应放在单独的实现单元或者
  `.scppo` object 里）同样没有任何可行路径能真正获得那个函数体：无论在
  同一个程序里的什么位置补上函数体——同一个 namespace 块，还是另外重新
  打开的 namespace 块——都会被当作 redefinition 拒绝，而不会被当作该声
  明自己的定义来接受；如果干脆不提供函数体，一旦被调用就会在链接阶段报
  undefined-reference 错误。
- 函数声明 / 定义上的 `inline` 目前只是 parser 级别的接受：scpp 接受这
  个关键字，因此像 `[[nodiscard]] inline SourceLocation
  make_source_location(...)` 这样的普通 C++ 签名现在可以通过解析，但它
  完全没有任何语义效果。它既不会提供真正 C++ 里那种“同一个函数可以跨多个
  translation unit 出现多个定义而不触发 ODR 违规”的放宽，也不会作为任
  何 codegen / optimization 的内联提示：标了 `inline` 的函数，其编译结
  果和运行行为都与不写 `inline` 完全相同。这是为了先打通编译器内部源码的
  self-hosting 准备工作而做的刻意简化；等到将来真的出现多 translation
  unit 的需求时，再补全完整的 `inline` 语义也不迟。
- coroutine / async 语言支持仍然缺失：还没有 `co_await`、`co_yield`、
  `co_return`，也没有 coroutine lowering / runtime integration。

## 库/标准库缺口（有价值，但不是当前重点）

这一节里的项目同样是真实缺口，但它们可以建立在“真实 C++ 编译器已经接受的语
法”之上，通过库 / runtime / tooling 来补齐。

- 还没有类似 `std::string_view` 的借用字符串视图。
- `std::span` 在核心数组场景之外的库表面仍然很窄：还没有 `std::string`
  互操作，也没有更广泛的容器 / 视图构造路径。
- 还没有像 `std::vector` 或哈希映射等价物这样的可增长标准容器。
- 还没有 `std::variant` / `std::visit` 风格的 tagged-union（带标签联合 / 和类
  型）库表面，因此这类底层能力在合法 C++ 语法里仍然缺失；但 scpp 应当通过库来
  追求它，而不是发明新语法。
- 并发支持仍然很浅：`std::thread` / `std::jthread` 已有，但还没有
  `std::mutex`、原子类型、条件变量或 future/promise 层。
- 文件 I/O 与文件系统支持仍然非常有限：已有面向 stdin 的
  `scpp::io::getline()`，但还没有通用的文件打开 / 读取 / 写入 API，也没有
  类似 `std::filesystem` 的路径层。
- 还没有 JSON / 文档解析与序列化库。
- 还没有归档 / 压缩 / 签名相关原语。
- 还没有密码学 / 哈希 / TLS 相关库表面。
- 仓库自带的 HTTP server 目前仍然只是静态文件服务器。它公开的 builder 只支
  持把 `/` 挂载到某个文件系统根目录，并切换少量文件分发选项；还没有通用的路
  由、请求处理或 API framework。
- 目前仍不现实做到 self-hosting：编译器后端直接绑定 LLVM 的 C++ API，而编译
  器实现本身也大量依赖 `std::shared_ptr` 等 scpp 目前并未自带提供的 C++ 标准
  库设施。
