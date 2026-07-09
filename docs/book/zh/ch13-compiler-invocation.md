# 编译器调用与 CLI

本章讲 scpp 编译器的实际命令行接口：怎样把一个程序编译成本地可执行文件，
常用 flag 分别干什么，以及除了普通的"编这个文件"之外，还保留了哪些别的
子命令。

## 13.1 默认的可执行文件构建方式

最普通的编译方式就是：

```sh
scpp file.scpp
```

这会把 `file.scpp` 直接编译并链接成一个本地可执行文件。

如果不给输出文件名，默认输出是：

```text
a.out
```

这跟 `clang`、`gcc` 等传统编译器沿用已久的约定一致。

如果想显式指定输出名，就传 `-o`：

```sh
scpp file.scpp -o myprogram
```

所以最常见的两种形式就是：

```sh
scpp hello.scpp
scpp hello.scpp -o hello
```

## 13.2 可执行文件构建路径的常用 flag

直接生成可执行文件的这条路径，常用 flag 如下。

### `-o <output>`

显式指定输出可执行文件路径：

```sh
scpp app.scpp -o app
```

不写 `-o` 时，输出默认是 `a.out`（见 [§13.1](#131-默认的可执行文件构建方式)）。

### `-I <dir>`

添加一个 module 搜索目录。

这是解析 `import mylib.math;` 时的图方便搜索机制：不需要在命令行上写出某个
具体文件路径。具体搜索规则本身见
[ch11 §11.14](ch11-modules-and-libraries.md#1114-import库-search-path)。

例子：

```sh
scpp app.scpp -I ./vendor/modules
```

### `--import name=path`

提供一个显式的 module 路径覆盖。

这是最完全显式的形式：不用目录搜索，直接告诉编译器，某个 module 名字应该
由哪一个 `.scppm` 或 `.scppkg` 来满足。具体解析规则同样见
[ch11 §11.14](ch11-modules-and-libraries.md#1114-import库-search-path)。

例子：

```sh
scpp app.scpp --import mylib.math=./pkg/mylib.math.scppm
```

### `--link <path>`

追加一个额外的原生链接输入。

当最终可执行文件除了 scpp 自己为当前程序及其 imported modules 产出的对象
之外，还需要再额外带上某个原生 object、archive 或其它 linker 可见输入时，
就用这个参数。

例子：

```sh
scpp app.scpp --link ./native/libhelper.a
```

### `--static`

请求生成一个完全静态链接的可执行文件。

例子：

```sh
scpp app.scpp --static -o app
```

它能不能真的成功，仍然取决于目标平台，以及所需系统库的静态版本是否实际可
用——这跟普通原生工具链本来的限制完全一样。

### `-g`

把 DWARF 调试信息发进输出二进制里。

例子：

```sh
scpp app.scpp -g -o app
```

这就是 LLDB 一类工具能做源码级调试的前提。至于 VS Code + CodeLLDB 的编辑
器/调试器配置本身，见 [ch12](ch12-ide-integration.md)。

## 13.3 诊断型子命令：`lex` 和 `parse`

还有两个保留为关键字子命令的路径，主要用于诊断和查看编译器内部中间结果：

```sh
scpp lex file.scpp
scpp parse file.scpp
```

- `lex`：打印 token 流。
- `parse`：打印 AST。

它们主要是给语言/编译器本身做检查和调试用的，不是平时构建一个可运行程序
的常规路径。

## 13.4 构建预编译 module 产物：`build-module`

生成 module 产物，跟生成一个可执行文件是另一件事，所以它仍然保留成一个显
式的关键字子命令：

```sh
scpp build-module file.scpp \
  --interface-out file.scppm \
  --archive-out file.scppa
```

这条命令产出的不是可执行文件，而是一对文件：

- 一个 `.scppm` module interface 文件
- 一个 `.scppa` 原生 archive，里面装着编译好的机器码

这套产物模型在 [ch11](ch11-modules-and-libraries.md) 有完整说明，尤其是
`.scppm` / `.scppa` / `.scppkg` 的讨论见
[§11.12](ch11-modules-and-libraries.md#1112-the-scppm-scppa-and-scppkg-formats)。

构建 module 的这条路径，也接受普通编译同样会用到的 import 相关 flag，
也就是 `-I <dir>` 和 `--import name=path`，用来解析构建该产物时所需的其
它 modules。

## 13.5 一页总结

主要命令形式如下：

```sh
scpp file.scpp
scpp file.scpp -o output
scpp file.scpp -g
scpp file.scpp -I dir --import name=path --link path --static
scpp lex file.scpp
scpp parse file.scpp
scpp build-module file.scpp --interface-out file.scppm --archive-out file.scppa
```

最值得记住的区分其实很简单：

- `scpp file.scpp ...`：生成可执行文件
- `scpp build-module ...`：生成可复用的 module 产物
- `scpp lex ...` / `scpp parse ...`：查看编译器内部中间形式

---

[← 上一章：IDE 集成](ch12-ide-integration.md) · [目录](README.md)
