# `.scppm` 模块接口格式

本文档定义 `.scppm`：单个 scpp module 接口的二进制形式——它的声明，
加上每个泛型函数体的一份序列化表示（第 2.1 节）。它不携带任何编译后的
机器码——一个 module 编译后的机器码是单独一份 `.scppa` 文件（`.scppo`
目标文件的原生归档），每个 target triple 一份，从外层的 `.scppkg` 包
里引用（见
[`.scppkg` 包格式](scppkg-format.md)）。

`.scppm` 被
[ch11 §11.12](../../book/zh/ch11-modules-and-libraries.md#1112-scppm-scppa-scppkg-三个格式)
引用。

跟 `.scppkg`（打包任意多个具名文件的包）不一样，一个 `.scppm` 文件永远
只有固定的两种可能内容——所以它不是 tar 归档。它的字节是一个固定头部，
紧跟着接口源码，以及（如果存在）泛型块。

## 1. 文件布局

```
[头部]                -- 固定大小，8 字节
[interface_length]    -- uint32，小端序，4 字节
[接口源码]             -- interface_length 字节：原始 UTF-8 源码文本
[generics_length]     -- uint32，小端序，4 字节；只在 flags bit 0 被置位时才存在
[泛型块]               -- generics_length 字节；只在 flags bit 0 被置位时才存在
```

### 头部

固定 8 字节头部：

| 偏移 | 大小 | 字段 | 取值 |
|---|---|---|---|
| 0 | 5 | `magic` | ASCII 字节 `SCPPM`。 |
| 5 | 1 | `major_version` | uint8。本文档定义主版本号 `1`。 |
| 6 | 1 | `patch_version` | uint8。本文档定义 patch 版本号 `0`。 |
| 7 | 1 | `flags` | bit 0、bit 7-1（见下）。 |

`flags` 的各个 bit：

| bit | 含义 |
|---|---|
| 0 | 接口源码后面有没有跟一个泛型块（第 2.1 节）。`0`：这个 module 没有导出任何泛型函数，下面 `generics_length`/泛型块相关字段都不存在。 |
| 7-1 | 保留，本版本恒为 `0`。 |

读取方先检查 `magic` 和 `major_version`，再解析其他任何东西。`magic`
不认识，或者 `major_version` 是读取方不支持的版本，在这一步就是一次
干净的解析失败——绝不会往文件后面继续走，崩溃或者靠猜测兜底。
`patch_version` 比读取方知道的更新，或者 `flags` 里有读取方不认识
含义的位，都不算错误：这两种情况只代表某个可选的、可以忽略的新增
内容——读取方对认不出来的东西直接忽略，照常继续处理。

一个 `.scppm` 文件自己的名字，去掉 `.scppm` 扩展名之后，就是这个
module 的点分名字（`mylib.math.scppm` 就是 module `mylib.math`）；这件
事不会在文件内部任何地方重复记录。

接口源码和泛型块都不压缩：`.scppm` 设计成可以被直接读取，包括作为本地
构建的中间产物。作为包的一部分分发的 `.scppm`，靠 `.scppkg` 自己信封的
压缩顺带被压缩（见
[`.scppkg` 包格式 第 1-2 节](scppkg-format.md)），而不是 `.scppm` 自己
压缩。

## 2. 接口与泛型内容

紧跟在头部之后：

| 字段 | 大小 | 取值 |
|---|---|---|
| `interface_length` | 4 字节，uint32 小端序 | 紧接着的接口源码的字节长度。 |
| 接口源码 | `interface_length` 字节 | 原始 UTF-8 文本：这个 module 的接口源文件。 |
| `generics_length` | 4 字节，uint32 小端序 | 只有 `flags` bit 0（第 1 节）被置位时才存在。紧接着的泛型块的字节长度。 |
| 泛型块 | `generics_length` 字节 | 只有 `flags` bit 0（第 1 节）被置位时才存在。这个 module 里每个泛型函数体的一份 scpp 内部序列化表示（第 2.1 节）。 |

一个 `.scppm` 文件对应的就是真实 C++ module 自己的 BMI（Clang 的
`.pcm`、GCC 的 `.gcm`、MSVC 的 `.ifc`），不多不少：接口声明，加上泛型
函数的可单态化函数体（第 2.1 节）——没有任何编译后的机器码，也没有依赖
或系统链接方面的元数据（真实 C++ module 也没有这两样，这个格式也没
有）。一个 module 编译后的机器码记在一份 `.scppa` 文件里（`.scppo`
目标文件的原生归档，每个 target triple 一份）；它需要哪些别的 module
或者系统库，是包管理层面的元数据，只记在外层 `.scppkg` 包的 manifest
里（见
[`.scppkg` 包格式 第 3 节](scppkg-format.md)）。两者都不会记在
`.scppm` 里。

一个 module 的接口源码里，有些函数可以给出完整函数体，有些可以不给
（无函数体、`extern`，见 ch11 §11.7）：有完整函数体的函数，在任何
target 上都能直接从接口源码编译；没有函数体的，只有在这个 module 针对
那个 target triple 的 `.scppa` 文件里打包的 `.scppo` 目标文件提供了
对应符号时，才能被链接。

### 2.1 泛型（concept 约束的）函数

一个泛型函数（[§5.11](../../book/zh/ch05-static-checks.md)）针对每个
被调用到的具体类型，各自在调用点自己的 build 里单独单态化——不像普通
函数，没有哪一份按 target 编译好的产物能覆盖所有调用方，因为调用方
可能会用到这个 module 的作者自己从没见过的具体类型。所以接口源码里
依然要给每个泛型函数写一条声明（无函数体，如下面例子）；但它的函数体
——调用方单态化时需要用到——不作为源码文本放在接口里，而是单独放在
泛型块里：

```cpp
double total_area(const Shape auto& a, const Shape auto& b);  // 声明在接口里，没有函数体
```

泛型块里存放的是每个泛型函数体的一份 scpp 内部序列化表示（不是 `.scpp`
源码文本），信息量足够消费者针对自己需要的具体类型和 target triple
做单态化、生成代码。这样能保持单态化零开销（没有 vtable，没有运行时
分发——跟 [§5.11](../../book/zh/ch05-static-checks.md) 一致），又不需要
把函数的逻辑以可读源码的形式分发出去。泛型块内部具体怎么编码，本文档
不做定义。

## 3. 可扩展性

新的可选字段可以通过升级 `patch_version`（第 1 节）引入；读取方对认不
出来的东西直接忽略即可。

`.scppm` 自己不携带任何签名：它是语言层面的格式（一个 module 编译后的
接口），不是分发格式。任何要交付给消费者的东西，完整性和来源校验完全
是 `.scppkg` 的事，对整个包做一次——见
[`.scppkg` 包格式 第 4 节](scppkg-format.md)。
