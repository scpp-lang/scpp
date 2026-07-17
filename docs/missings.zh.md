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
- 大部分 ISO `alignas` / `alignof(type-id)` 支持已经落地，但文件作用域上的
  变量声明只要写 `alignas` 仍会被拒绝；同样的写法放在局部变量或 class 声明上则
  已经可以工作。
- 规范现在已经显式允许在 `requires(...)` probe parameter 上使用具名生命周期组和
  `[[scpp::lifetime(any)]]`；但编译器仍然还不能在这个 probe-parameter
  位置解析该 attribute。
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
