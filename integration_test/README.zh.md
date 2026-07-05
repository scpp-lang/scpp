# scpp 集成测试

> English version: [README.md](README.md)

这个目录是 `scpp` 编译器的**黑盒**集成测试套件。它与 `src/`（实现）和
`docs/book/`（语言规范）相互独立地维护：这里的测试完全通过阅读 `docs/book/`
编写，并把编译好的 `scpp` CLI 当作外部工具来调用——就像语言的普通使用者一样，
不依赖、也不需要了解 scpp 编译器的内部模块。

## 工作原理

- `cases/<NN_category>/<name>.cpp` —— 一段展示某条文档化语言规则的小 scpp
  程序（文件顶部注释会引用 `docs/book/en/chXX-*.md` 里对应的章节）。
- `cases/<NN_category>/<name>.expected` —— 如果 `scpp` 正确实现了规范，这段
  程序*应该*产生的结果。有三种形式：
  1. **第一行是一个数字**：`scpp build` 必须编译成功，运行生成的可执行文件
     必须以这个退出码结束（0-255，遵循 POSIX `WEXITSTATUS`/shell `$?` 的语义
     ——被信号杀死的进程会被归一化为 `128+信号数`，例如 SIGABRT -> 134）。
     第一行之后的内容是期望的 stdout，逐字节比对。
  2. **`COMPILE_ERROR`**：`scpp build` 必须以一个干净、为正数的退出码失败
     （一个真正的诊断信息，而不是崩溃）。具体的错误文案不做检查——规范并未
     锁定措辞。
  3. **`NO_ABORT`**：仅用于极少数场景——某个 scpp 插入的运行时检查（span
     边界检查、溢出检查）被有意地*跳过*了（在 `unsafe { }` 内部/一个 native
     函数中，见 ch01 §1.3），因此读到/算出的值本身就是不确定的垃圾值，没法
     固定下来断言——但进程仍必须正常终止（return/exit），而不是被信号杀死。

测试运行器本身（`run_tests.cpp`）是一个小巧、无外部依赖的 C++ 程序——只用了
POSIX `fork`/`exec` + `<filesystem>`，没有第三方库，也没有链接任何 scpp 模块。
它有自己独立的 `CMakeLists.txt`（与顶层 scpp 项目彻底独立——不用
`add_subdirectory`，不需要 LLVM/模块依赖）。先构建一次：

```sh
cmake -S . -B build
cmake --build build
```

然后运行整个套件（会在配置期把 `scpp` 二进制的默认路径 `../build/scpp` 烘焙
进去，所以无论在哪个工作目录下运行都没问题）：

```sh
./build/run_tests
```

只运行某一类，或按子串过滤：

```sh
./build/run_tests 05_span
./build/run_tests bool_and_char
```

传 `--scpp-bin <path>` 可以指定使用另一个构建产物。

## 分类

| 目录 | 覆盖内容 |
|---|---|
| `01_basics` | M1：标量、局部变量、`if`/`while`、函数、算术、零初始化 |
| `02_structs` | `struct` 平凡性规则、零初始化、按位拷贝、禁止的成员类型 |
| `03_unique_ptr` | `std::make_unique`/`std::move`、移出检查、箭头语法糖 |
| `04_references_borrow` | `T&`/`const T&`、alias-XOR-mutability、NLL 借用释放、生命周期省略 |
| `05_span` | `std::span<T>` 的构造/下标/边界检查 |
| `06_unsafe_blocks` | `unsafe { }` 的门控与作用域规则 |
| `07_extern_c` | `extern "C"` 声明/定义、真实 libc 互操作 |
| `08_address_of` | `&expr`、`const T*`/`T*` 的区分 |
| `09_integer_overflow` | `safe` 下检查并 abort、`unsafe` 下环绕、除法/取模的特殊情形 |
| `10_bool_and_char` | 标量间无隐式转换、逻辑运算符的短路求值 |
| `11_safe_unsafe_boundary` | ch02 中 safe/unsafe 的调用方向表 |
| `12_struct_vs_class` | `struct` 与 `class` 在访问控制上的分歧 |
| `13_unsupported_robustness` | 尚未实现的语法能干净地报错，不会崩溃 |
| `14_classes` | 构造/析构函数、私有成员访问控制、无拷贝语义、方法调用的借用检查、`this` |
| `15_function_overloading` | 按精确类型匹配解析重载、by-value/by-reference 独立轴、const/非-const 方法 |

## 测试理念

- 每个 `.cpp` 文件都力求**严格符合 `docs/book/`**——如果某个测试失败了，先去
  查它引用的文档章节。如果测试本身其实违反了规范，就修正测试。如果测试确实
  符合规范却依然失败，那就是实现上的 bug，记录在这里留给 `src/` 的维护者去
  修——这个套件本身不会为了绕开失败而去改 `src/`。
- 程序优先通过**进程退出码**（`main` 的返回值）来观察行为；如果要验证真实的
  C 互操作，就使用通过 `extern "C"` 声明的真实 libc 调用（`puts`、
  `printf`）——这两者都是文档里明确记载的机制。`tests/test_source` 使用的那些
  仅供内部测试用的辅助函数（例如 `print_int`/`print_bool`/`print_char`）在这里
  被刻意**不予使用**，因为它们不属于文档记载的语言表面。
- 每个函数体（包括返回 `void` 的）都需要显式的 `return` 语句——scpp 目前没有
  隐式地"落到函数末尾就返回"这回事，尽管 `docs/book/` 里并没有明确点出这一点。
- `docs/book/` 有时会落后于 `src/`（两者是独立维护的）——某个章节写着"尚未
  实现"，不代表现在依然如此。拿不准的时候，用 `scpp build` 快速探测一下就能
  确认。

## 现状

123/123 通过（对应提交 `274a1a8`）。`14_classes` 和 `15_function_overloading`
是在 `class` 访问控制/拷贝限制（ch04 §4.2/ch05 §5.9）以及函数重载
（ch05 §5.10）被实现之后新增的；`mutable`（第一阶段的内部可变性，ch04
§4.2/ch08 Q4）和 `namespace`/多文件模块（ch11）仍然只是设计，尚未实现，
`13_unsupported_robustness` 里确认它们目前仍能被干净地拒绝。每次 `src/` 或
`docs/book/` 有变动后，重新跑一遍 `./build/run_tests` 来发现回归。
