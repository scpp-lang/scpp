# `.scppkg` 包格式

本文档定义 `.scppkg`：把一个或多个 scpp module 打包起来分发的包——里面
可以是原始的 `.scpp` 接口源码，也可以是一个 `.scppm` 接口（见
[`.scppm` 模块接口格式](scppm-format.md)）配上每个 target triple 一份
`.scppa` 归档，或者两者混合。

`.scppkg` 被
[ch11 §11.12](../../book/zh/ch11-modules-and-libraries.md#1112-scppm-scppa-scppkg-三个格式)
引用。

`.scppkg` manifest（第 3 节）里提到的 `.scppa` 文件，不是本文档定义的
格式：它就是目标平台自己的原生静态库归档格式（Unix 是 `ar` 格式的
`.a`，Windows 是 `.lib`），里面打包了这个 module 每个贡献代码的文件——
它的主接口单元、某个实现单元、或者某个 partition（见 ch11
[§11.3](../../book/zh/ch11-modules-and-libraries.md#113-导出面与接口实现单元的拆分)
和
[§11.4](../../book/zh/ch11-modules-and-libraries.md#114-module-partition)）
——针对某个 target triple 各自编译出的一份 `.scppo` 目标文件成员。因为
它是原生归档，系统 linker 会直接读取一个 `.scppa` 文件，跟处理任何别的
静态库完全一样；除了里面具体包含哪些 `.scppo` 成员之外，没有任何
scpp 专属的东西——里面的成员甚至不需要叫 `.scppo` 这个名字（linker
靠归档自带的索引定位符号，不看成员名字）。

`.scppo` 文件本身，同样不是本文档定义的格式：它就是目标平台自己的原生
目标文件格式（ELF、COFF、Mach-O 等）。

## 1. 信封

一个 `.scppkg` 文件用一个信封包一个
[tar](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html)
归档：

```
[头部]              -- 固定大小，8 字节，永远原样存储、不压缩
[payload_length]    -- uint64，小端序，8 字节
[Payload]           -- payload_length 字节：一个 tar 归档，按头部的 flags 决定原样存储还是单个 LZMA 流
[签名块]             -- 可选，在末尾，只在 flags bit 4 被置位时才存在（第 4 节）
```

### 头部

固定 8 字节头部，永不压缩：

| 偏移 | 大小 | 字段 | 取值 |
|---|---|---|---|
| 0 | 5 | `magic` | ASCII 字节 `SCPPK`。 |
| 5 | 1 | `major_version` | uint8。本文档定义主版本号 `1`。 |
| 6 | 1 | `patch_version` | uint8。本文档定义 patch 版本号 `0`。 |
| 7 | 1 | `flags` | bit 3-0、bit 4、bit 7-5（见下）。 |

`flags` 的各个 bit：

| bit | 含义 |
|---|---|
| 3-0 | payload 的压缩格式：`0` = 原样存储；`1` = 一个单独的 LZMA 流（第 2 节）。 |
| 4 | 末尾有没有一个签名块（第 4 节）。本版本恒为 `0`：不存在这个块。 |
| 7-5 | 保留，本版本恒为 `0`。 |

读取方先检查 `magic` 和 `major_version`，再解析其他任何东西。`magic`
不认识，或者 `major_version` 是读取方不支持的版本，在这一步就是一次
干净的解析失败——绝不会往文件后面继续走，崩溃或者靠猜测兜底。
`patch_version` 比读取方知道的更新，或者 `flags` 里有读取方不认识
含义的位，都不算错误：这两种情况只代表某个可选的、可以忽略的新增
内容——读取方对认不出来的东西直接忽略，照常继续处理。

### Payload

紧跟在头部之后：

| 字段 | 大小 | 取值 |
|---|---|---|
| `payload_length` | 8 字节，uint64 小端序 | 紧接着的 payload 存储时的字节长度（如果 `flags` 选择 lzma，就是压缩后的大小；否则就是原始字节长度）。 |
| Payload | `payload_length` 字节 | 如果 `flags` 选择不压缩，就是原样的 tar 字节；如果选择 lzma，就是一个单独的 LZMA 流（第 2 节）。解码后得到一个 tar 归档，内容结构见第 3 节。 |

读取方如果只想要这个 tar 归档，只需要在一个固定位置（紧跟在头部和
`payload_length` 字段之后）读 `payload_length` 个字节，不需要再往下读——
不管末尾有没有签名块（第 4 节），因为那个块永远是从 payload 开始算
`payload_length` 字节之后的位置开始，读取方不需要看 payload 之后的
任何内容就已经知道这个位置。

## 2. LZMA 流格式

当 `flags`（第 1 节）选择 lzma 时，payload 存储的字节是一个原始 LZMA
流：一个 5 字节的属性头（字节 0 打包 `lc`/`lp`/`pb`，后面跟 4 字节
小端序的字典大小），后面跟一个 8 字节小端序的、给出解码后 tar 归档
字节长度的未压缩大小字段，再后面是 LZMA 压缩数据。这正是 public
domain 的 LZMA SDK 自带的简单编码器（`LzmaCompress`）产出的格式。上面
不叠加 `.xz`/`.7z` 或任何其他容器封装。

## 3. `.scppkg` 的内容

解码 payload（第 1 节）得到一个 tar 归档，里面装着：

| 路径 | 内容 |
|---|---|
| `MANIFEST.json` | 这个包的 manifest（见下）。 |
| module 条目里 `interface` 指向的路径 | 一个完整的、嵌套的 `.scppm` 文件（见 [`.scppm` 模块接口格式](scppm-format.md)）。 |
| module 条目里 `archives[].path` 指向的路径 | 一份 `.scppa` 文件：这个 module 针对某个 target triple 编译出的 `.scppo` 目标文件的原生归档。 |
| module 条目里 `path` 指向的路径 | 一个原始的 `.scpp` 接口源文件（只有 `kind: "source"` 的条目才有）。 |

`MANIFEST.json`：

```json
{
  "schema_version": "1.0",
  "package": { "name": "mylib", "version": "1.2.0" },
  "dependencies": [
    { "name": "otherlib", "version": "^1.0.0" }
  ],
  "modules": {
    "mylib.math": {
      "kind": "binary",
      "interface": "mylib.math.scppm",
      "archives": [
        { "target_triple": "x86_64-linux-gnu",
          "path": "mylib.math.x86_64-linux-gnu.scppa" }
      ],
      "native_link_requirements": ["m"]
    },
    "mylib.collections": { "kind": "source", "path": "mylib.collections.scpp" }
  }
}
```

| 字段 | 含义 |
|---|---|
| `schema_version` | 这份 manifest schema 自己的 `"major.minor"` 版本（第 4 节），跟信封的 `major_version`/`patch_version`（第 1 节）、以及任何嵌套 `.scppm` 自己的版本都相互独立。本文档定义 `"1.0"`。要在解析 manifest 里其它任何字段之前先读取并校验它。 |
| `package.name` | 这个包的名字。自由格式。 |
| `package.version` | 这个包的版本。自由格式。 |
| `dependencies` | 这个包需要的其它包列表（见下）。作者自己提供的包管理元数据；`version` 怎么匹配/解析，本文档不做定义。 |
| `modules.<点分 module 名字>.kind` | `"binary"`（这个条目有 `interface` 和 `archives`）或者 `"source"`（这个条目有 `path`）。 |
| `modules.<点分 module 名字>.interface` | 只有 `kind` 是 `"binary"` 时才有。嵌套 `.scppm` 文件的 tar 内部路径。 |
| `modules.<点分 module 名字>.archives` | 只有 `kind` 是 `"binary"` 时才有。这个 module 的 `.scppa` 文件列表，每个 target triple 一份（见下）。 |
| `modules.<点分 module 名字>.path` | 只有 `kind` 是 `"source"` 时才有。原始 `.scpp` 接口源文件的 tar 内部路径。 |
| `modules.<点分 module 名字>.native_link_requirements` | 这个 module 编译出的代码在链接期依赖的系统库（`-l` 风格的名字）。作者自己提供；没有就不写这个字段。 |

每个 `dependencies`条目：

| 字段 | 含义 |
|---|---|
| `name` | 那个包的名字。 |
| `version` | 那个包的版本要求。对本文档不透明。 |

每个 `archives`条目：

| 字段 | 含义 |
|---|---|
| `target_triple` | 这份 `.scppa` 文件针对的 target triple。 |
| `path` | 这份 `.scppa` 文件的 tar 内部路径。 |

一个 module 就算由好几个文件贡献代码（它的主接口单元、一个或多个实现
单元、一个或多个 partition——见 ch11
[§11.3](../../book/zh/ch11-modules-and-libraries.md#113-导出面与接口实现单元的拆分)
和
[§11.4](../../book/zh/ch11-modules-and-libraries.md#114-module-partition)），
每个 target triple 依然只有**一条** `archives` 条目：`.scppa` 文件本身
就是一份原生归档，里面打包了这个 module 在那个 triple 下需要的所有
`.scppo` 成员，所以这份 manifest 不需要逐个列出它们。

`dependencies` 和 `native_link_requirements` 都是包管理层面的元数据，
不是语言层面的事实：`.scppm` 文件两者都不带，因为真实 C++ module 也
没有这两个概念的对应物。`dependencies` 是整个包一份、汇总记录——这跟
其它包生态的惯例一致（按包为粒度解析依赖，不按文件/module 分别记）；
`native_link_requirements` 则按 module 分别记录，因为不同 module 编译
出的代码可能需要不同的系统库。

同一个包可以把同一个 module 既列一份 `kind: "source"`、又（在另一个
条目下）列一份 `kind: "binary"`，供消费者自己选；如果两份都存在，本
文档不要求它们在包里其它 module 名字的问题上互相一致。

## 4. 可扩展性

`schema_version`（第 3 节）是一个 `"major.minor"` 字符串，给 manifest
schema 单独计版本，跟信封自己的 `major_version`/`patch_version`（第 1
节）相互独立。读取方要先检查它的 major 部分，再信任 manifest 里其它
内容；major 部分读取方不支持，就是一次干净的解析失败，在读取任何其它
manifest 字段之前就报出来。minor 部分比读取方知道的更新，不算错误——
只代表有些读取方不认识的可选字段，读取方忽略它们、继续处理即可。新的
可选 `MANIFEST.json` 字段通过升级 `schema_version` 的 minor 号引入；对
schema 的破坏性变更——删掉一个字段，或者改变一个已有字段的含义——则
需要升级 major 号。

信封自己的 `major_version`/`patch_version`（第 1 节）给字节容器本身计
版本，跟 `schema_version` 相互独立：它们管的是第 1 节描述的头部/
payload/签名这层封装，不管 payload 内部嵌套的 manifest schema。

对信封的头部和 payload 一起做的签名（这样谁都不能被单独篡改）不放在
payload 自己的 tar 归档里面。它是一个尾部的块，紧跟在 payload 之后，
只有 `flags` bit 4（第 1 节）被置位时才存在：

| 字段 | 大小 | 取值 |
|---|---|---|
| `signature_length` | 8 字节，uint64 小端序 | 紧接着的签名的字节长度。 |
| 签名 | `signature_length` 字节 | 对本文档不透明；覆盖头部和 payload 的字节一起。 |

本版本 bit 4 恒为 `0`：不存在这个块，文件在 payload 结束之后就完了。
因为读取方只靠 `payload_length`（第 1 节）就能确定 payload 到哪里
结束，一个还不认识这个块的读取方，照样能正确读出 payload，跟它后面
存不存在签名块无关。

签名本身的格式、以及验证方怎么确定该信任哪把密钥/证书，本文档都不做
定义。
