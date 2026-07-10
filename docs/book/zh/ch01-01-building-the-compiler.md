# 构建编译器

scpp 目前是从源码构建的。如果你已经把仓库检出到本地，第一步就是先产出一个可用的
`scpp` 二进制。

## 你需要准备什么

从源码构建时，请准备：

- CMake 3.28 或更新版本
- Ninja
- Clang/LLVM 22
- SQLite 开发头文件与库
- zstd 开发头文件与库

在 Debian 或 Ubuntu 上，可以这样安装：

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libsqlite3-dev libzstd-dev
```

## 配置并构建

在仓库根目录运行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build
```

命令完成后，刚构建出来的编译器在这里：

```text
./build/scpp
```

## 可选：把它安装到你的 `PATH` 里

如果你希望这一章后面的命令直接写 `scpp`，而不是每次都写 `./build/scpp`，可以把
构建结果安装到你自己控制的前缀目录里：

```sh
cmake --install build --prefix "$HOME/.local/scpp"
export PATH="$HOME/.local/scpp/bin:$PATH"
```

这个安装步骤会生成一棵自包含目录，里面同时包含编译器本体以及它需要的 stdlib 文
件。

如果你暂时不想安装，也完全没关系。你仍然可以继续在仓库根目录里直接使用
`./build/scpp`。

## 现在你已经有了什么

到这里，你已经拥有了一个真实可用的编译器二进制。下一节我们就拿它去构建最小的
scpp 程序。

---

[← 上一节：开始上手](ch01-00-getting-started.md) · [目录](README.md) · [下一节：Hello, World! →](ch01-02-hello-world.md)
