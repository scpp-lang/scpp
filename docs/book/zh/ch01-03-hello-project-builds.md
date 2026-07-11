# 第一个 project build

单文件编译非常适合快速试验。但只要你想要一个有名字的二进制程序、一个真正的项目
目录，scpp 的 manifest-based build 模式就会更顺手。

创建一个目录，然后把顶层的 `scpp.toml` 和放在 `src/` 下面的 `main.scpp` 准备好。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "starter"
version = "0.1.0"

[[bin]]
name = "hello"
root = "src/main.scpp"
sources = ["src/**/*.scpp"]
```

`src/main.scpp`：

```cpp
import std;

int main() {
    std::println("Hello from a project build!");
    return 0;
}
```

在这个目录里运行：

```sh
scpp build
./.scpp/build/*/dev/starter/hello
```

输出：

```text
Hello from a project build!
```

输出路径里的 `*` 会展开成你的 target triple，例如 `x86_64-pc-linux-gnu`。scpp
会把构建产物放进 `.scpp/build/` 下面，因此项目目录本身会保持得比较小、也比较可
预测。

到这里，第一章的目标就已经完成了：

- 你构建了编译器；
- 你编译了一个单文件程序；
- 你构建了一个带 manifest、带具名二进制目标的项目。

下一章会继续保持“动手做”的节奏，但重点会从“把工具跑起来”切换到“写一个稍微
更完整一点的程序”，开始把变量、循环和条件组合起来使用。

---

[← 上一节：Hello, World!](ch01-02-hello-world.md) · [目录](README.md)
