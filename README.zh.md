# scpp

一个编译器前端，目标语言**看起来就是原汁原味的现代 C++**，只加入极少量
扩展——全都拼写成 `scpp` 命名空间下的 attribute，所以 scpp **零新增
关键字**，核心是 `[[scpp::unsafe]]`。每个函数默认都启用 **Rust 式健全
（sound）的编译期安全检查**（所有权、借用、生命周期）；
`[[scpp::unsafe]] { }` 语句块局部放宽检查器本身无法验证的一小撮固定
操作。后端经 **LLVM** 生成本地二进制。

English README: [`README.md`](README.md)

## 设计理念

- **视觉上就是 C++。** 任何熟悉现代 C++ 的人第一眼就应该认为这是 C++。
- **加法极简，零新增关键字。** 只在绝对必要时引入新语法，而且一律拼写成
  `scpp` 命名空间下的 attribute。核心新增只有 `[[scpp::unsafe]]`。
- **复用已知语法、重新赋予语义。** `std::move()`、`T&`、`unique_ptr`、`span`
  等既有写法，被无条件地、在任何地方赋予更强的静态含义，但不改变它们对
  用户的"外观"。
- **安全是默认状态；`[[scpp::unsafe]]` 是唯一的退出方式，而且是局部的、
  可组合的。** `[[scpp::unsafe]] { }` 只放宽一份固定、列举出来的操作
  清单——它从来不是一个能把整个函数或整个文件检查关掉的开关。
- **健全性优先于兼容性。** 宁可暂时"尚未支持"某语法，也不放行一个不健全
  的检查。100% C++ 兼容不是目标。

## 示例

```cpp
int sum(std::span<const int> v) {   // 默认受检查：所有权、借用、生命周期
    int acc = 0;
    int i = 0;
    while (i < v.size) {
        acc += v[i];   // 默认带边界检查
        i = i + 1;
    }
    return acc;
}

int legacy(int* p) {
    [[scpp::unsafe]] {
        return *p;   // 裸指针解引用需要显式的 [[scpp::unsafe]] 块
    }
}
```

## 文档

完整语言规范是《The SCPP Programming Language》一书，采用中英双语维护：

- English: [`docs/book/en/README.md`](docs/book/en/README.md)
- 中文: [`docs/book/zh/README.md`](docs/book/zh/README.md)

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

早期设计阶段。

- **M1 — 最小端到端管线**（标量 + 局部变量 + 控制流 + 函数 → AST → LLVM IR
  → 可执行文件，暂不含安全检查）：已完成。
- **M2 — 类型系统 + `struct` + `unique_ptr` + move 语义**（内存布局固定
  遵循 Clang ABI 的纯平凡 `struct`；`std::move` 作为编译器识别的 move
  hint；move-out 检查，move 出去的 `std::unique_ptr` 不能再被读取）：
  已完成。
- **M3 — MIR + 初始化检查 + drop 插入**（基于 CFG 的 MIR；两阶段 worklist
  数据流分析做 move/初始化检查，在 codegen（真正的按作用域插入
  `std::unique_ptr` drop）和 move-checker 两侧都具备词法作用域感知）：
  已完成。
- **M4 — borrow 与 alias-XOR-mutability 检查**（仅函数内，第一阶段）：
  局部引用变量和函数引用参数支持 `T&`（可变/独占借用）和 `const T&`
  （共享借用），通过按变量记录的借用格来强制 alias-XOR-mutability；借用
  期长遵循词法作用域（尚非 NLL 风格的生命周期分析，那是 M5 的工作）。
  引用指向 `std::unique_ptr`、函数返回引用、以及引用类型的 struct 字段
  暂不支持，留待后续版本。

完整路线图见[本书](docs/book/zh/README.md)的里程碑章节。

## 许可

依据 [The Unlicense](LICENSE) 释放至公有领域。
