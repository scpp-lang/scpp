# IDE 集成

本章讲的是 scpp 程序**源码级调试**的实用做法：如何在 VS Code 里把断点、
单步、回溯和局部变量查看真正跑起来。

关键前提是：直接用 `scpp` 调用一个源码文件时，只要带上 `-g`，就可以给生成出来的本地二进制带上**真实的 DWARF
调试信息**（完整调用方式见 [ch13](ch13-compiler-invocation.md)）。只要这份调试信息在，基于 LLDB 的常规工具链就能像调试任何其
它 LLVM 产出的可执行文件一样，去调试 scpp 程序。

## 12.1 用 `-g` 生成调试信息

构建要调试的程序时，加上 `-g`：

```sh
scpp foo.scpp -o foo -g
```

`-g` 的作用，是让 `scpp` 在输出二进制里发出调试元数据。实际效果就是，
下面这些事才真正成立：

- 按源码行下断点
- 按语句单步执行
- 查看当前作用域里的局部变量
- 打印带真实源码位置的回溯

没有 `-g` 时，程序照样能运行；只是调试器手里可用的源码级信息会少得多。

## 12.2 VS Code：推荐的调试器选择

目前在 VS Code 里最省事的方案，是安装 **CodeLLDB** 扩展：

- 扩展 ID：`vadimcn.vscode-lldb`
- 商店名称：**CodeLLDB**

推荐它的原因很简单：它自带 LLDB，而且本来就能很好地处理基于 LLVM/DWARF
的二进制，所以额外配置最少。

## 12.3 最小可用的 `launch.json`

在工作区里创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "lldb",
      "request": "launch",
      "name": "Debug scpp program",
      "program": "${workspaceFolder}/myprogram",
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

之后按下面的顺序操作：

1. 先用 `-g` 构建二进制
2. 在 VS Code 里打开工作区
3. 在源码文件里下断点
4. 启动 `Debug scpp program` 这条调试配置

`program` 记得改成你实际构建出来的那个二进制路径。

## 12.4 `.scpp` 文件默认没有断点槽：问题出在哪

这里有一个很关键、也很容易卡住人的 VS Code 细节。

默认情况下，`.cpp` 文件通常一打开就能看到断点槽（breakpoint gutter），
但 **`.scpp` 文件不行**。问题并不在调试器本身：只要二进制是带 `-g`
构建出来的，LLDB 这边完全能正确调试。真正的问题出在**编辑器侧的断点槽**。

VS Code 只有在某个语言模式，被某个已安装的调试扩展声明为支持断点时，
才会显示/允许普通源码断点。CodeLLDB 已经注册了 `cpp`、`c`、`rust`
等常见语言，但它自然不知道一个单独的 `scpp` 语言模式。与此同时，
`debug.allowBreakpointsEverywhere` 的默认值又是 `false`，所以 VS Code
不会给未知语言 ID 显示断点槽。

## 12.5 推荐修复：把 `*.scpp` 关联成 `cpp`

推荐做法，是在 `.vscode/settings.json` 里加一个**工作区局部**的文件关联：

```json
{
  "files.associations": {
    "*.scpp": "cpp"
  }
}
```

这样一来，VS Code 会在工具链层面把 `*.scpp` 当成语言 ID `cpp` 处理。由
于 CodeLLDB 本来就把 `cpp` 视为支持断点的语言，`.scpp` 文件里也就会出
现断点槽，普通的行断点也就能正常工作。

这还有一个顺手的副作用：文件会获得近似 C++ 的语法高亮，而这对 scpp 源
码本来也基本合适。

## 12.6 为什么推荐这个修复，而不是全局放开

另一个可选办法，是打开全局设置：

```json
{
  "debug.allowBreakpointsEverywhere": true
}
```

它当然也能工作，但这是一把更钝的锤子：它会给**所有**文件类型都打开断点
槽，而不是只针对 scpp。因此，默认推荐仍然应该是 `files.associations`
这条路：

- 作用范围只限于 `*.scpp`
- 直接复用 CodeLLDB 已有的 `cpp` 断点支持
- 还能顺带改善语法高亮

## 12.7 一套最小可用的端到端配置

一套实际可用的最小配置如下：

1. 安装 **CodeLLDB**（`vadimcn.vscode-lldb`）。
2. 带调试信息构建：
   ```sh
   scpp myprogram.scpp -o myprogram -g
   ```
3. 添加 `.vscode/launch.json`，写入一条 `lldb` 启动配置。
4. 添加 `.vscode/settings.json`，内容如下：
   ```json
   {
     "files.associations": {
       "*.scpp": "cpp"
     }
   }
   ```
5. 打开 `myprogram.scpp`，下断点，然后启动调试器。

这些都配好以后，VS Code 就能为 scpp 程序提供普通的源码级调试体验：断点、
单步、变量查看，以及回溯。

---

[← 上一章：模块与库](ch11-modules-and-libraries.md) · [目录](README.md) · [下一章：编译器调用与 CLI →](ch13-compiler-invocation.md)
