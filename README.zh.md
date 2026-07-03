# scpp

一门"看起来就是原汁原味现代 C++"的语言，仅加入极少量扩展（核心是 `safe`
关键字）。被 `safe` 标注的区域启用 **Rust 式健全（sound）的编译期安全检查**
（所有权、借用、生命周期）；其余代码按普通 C++ 语义处理。后端经 **LLVM**
生成本地二进制。

## 设计理念

- **视觉上就是 C++。** 任何熟悉现代 C++ 的人第一眼就应该认为这是 C++。
- **加法极简。** 只在绝对必要时引入新语法。核心新增仅 `safe` / `unsafe`。
- **复用已知语法、重新赋予语义。** `std::move()`、`T&`、`unique_ptr`、`span`
  等既有写法，在 `safe` 区被赋予更强的静态含义，但不改变它们对用户的"外观"。
- **安全是可选的、局部的、可组合的。** 未标注的代码保持完全的 C++ 自由
  （和不安全）。
- **健全性优先于兼容性。** 在 `safe` 区内，宁可暂时"尚未支持"某语法，也不
  放行一个不健全的检查。100% C++ 兼容不是目标。

## 示例

```cpp
safe int sum(const std::vector<int>& v) {   // 此函数体启用检查
    int acc = 0;
    for (auto x : v) acc += x;
    return acc;
}

int legacy() {                               // 普通 C++，不做检查
    int* p = nullptr; return *p;             // 允许；后果自负
}
```

## 文档

语言规范采用中英双语维护：

- English: [`docs/language-spec-v0.1.en.md`](docs/language-spec-v0.1.en.md)
- 中文: [`docs/language-spec-v0.1.zh.md`](docs/language-spec-v0.1.zh.md)

## 构建

需要支持 C++23 modules 的 Clang、CMake 3.28+、Ninja，以及 LLVM 开发包。
在 Debian/Ubuntu 上：

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libzstd-dev
```

（需要 `libzstd-dev` 是因为 LLVM 的 CMake config 会链接它；不装的话
`find_package(LLVM)` 会报缺少 `zstd::libzstd_shared` target。）

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<LLVM 的 lib/cmake/llvm 路径>
cmake --build build
ctest --test-dir build
```

`scpp` CLI 支持：

```sh
scpp lex <file>              # 打印 token 流
scpp parse <file>            # 打印 AST
scpp build <file> [-o <out>] # 通过 LLVM 编译为本地可执行文件
```

## 状态

早期设计阶段。里程碑 **M1 — 最小端到端管线**（标量 + 局部变量 + 控制流 +
函数 → AST → LLVM IR → 可执行文件，暂不含 `safe` 检查）已实现：词法分析器、
解析器/AST，以及能编译并链接出本地可执行文件的 LLVM 后端。完整路线图见
规范中的里程碑章节。

## 许可

依据 [The Unlicense](LICENSE) 释放至公有领域。
