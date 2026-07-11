# Hello, World!

现在你已经有了编译器，接下来先让它做一件肉眼可见的事。

创建一个名为 `hello.scpp` 的文件：

```cpp
import std;

int main() {
    std::println("Hello, world!");
    return 0;
}
```

如果你在上一节里已经把 `scpp` 安装到了 `PATH` 上，可以这样构建并运行：

```sh
scpp hello.scpp
./a.out
```

如果你仍然是在仓库检出目录里直接使用刚构建出的编译器，那么把命令改成
`./build/scpp hello.scpp` 就可以。

输出：

```text
Hello, world!
```

这里用到的 `std::println` 来自 scpp 的标准库，用来打印一整行文本。

这个极小程序里其实已经藏着几个重要概念：

- `int main()` 是程序入口；
- `import std;` 让这个文件可以使用标准库；
- 整个程序在表面上依然看起来像普通 C++，这正是 scpp 最核心的设计目标之一。

下一节里，我们会保持程序同样很小，但把它放进一个真正的 manifest-based project。

---

[← 上一节：构建编译器](ch01-01-building-the-compiler.md) · [目录](README.md) · [下一节：第一个 project build →](ch01-03-hello-project-builds.md)
