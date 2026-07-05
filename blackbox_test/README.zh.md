# scpp 黑盒测试

> English version: [README.md](README.md)

这个目录是 `scpp` 编译器的**黑盒**测试套件。它与 `src/`（实现）和
`docs/book/`（语言规范）相互独立地维护：这里的测试完全通过阅读 `docs/book/`
编写，并把编译好的 `scpp` CLI 当作外部工具来调用——就像语言的普通使用者一样，
不依赖、也不需要了解 scpp 编译器的内部模块。

## 工作原理

- `cases/<NN_category>/<name>.scpp` —— 一段展示某条文档化语言规则的小 scpp
  程序（文件顶部注释会引用 `docs/book/en/chXX-*.md` 里对应的章节）。scpp
  源文件用 `.scpp` 后缀，不用 `.cpp`（ch08 Q7/Q13）：既然现在每个函数默认都
  无条件被检查，就绝不能让一个普通的 `.cpp` 文件被悄悄喂给 scpp 编译器、在
  作者没有要求的情况下被检查。
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
     边界检查、溢出检查）在一个 `unsafe { }` 块内被有意地*跳过*了（见
     ch01 §1.1），因此读到/算出的值本身就是不确定的垃圾值，没法固定下来
     断言——但进程仍必须正常终止（return/exit），而不是被信号杀死。

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
| `01_basics` | M1：标量、局部变量、`if`/`while`、函数、算术、零初始化、默认无条件检查 |
| `02_structs` | `struct` 平凡性规则、零初始化、按位拷贝、禁止的成员类型 |
| `03_unique_ptr` | `std::make_unique`/`std::move`、移出检查、箭头语法糖 |
| `04_references_borrow` | `T&`/`const T&`、alias-XOR-mutability、NLL 借用释放、生命周期省略 |
| `05_span` | `std::span<T>` 的构造/下标/边界检查 |
| `06_unsafe_blocks` | `unsafe { }` 的门控与作用域规则；§5.1-§5.4 在其内部依然生效 |
| `07_extern_c` | `extern "C"` 声明/定义、真实 libc 互操作 |
| `08_address_of` | `&expr`、`const T*`/`T*` 的区分 |
| `09_integer_overflow` | 默认检查并 abort、`unsafe` 下环绕、除法/取模的特殊情形 |
| `10_bool_and_char` | 标量间无隐式转换、逻辑运算符的短路求值 |
| `12_struct_vs_class` | `struct` 与 `class` 在访问控制上的分歧 |
| `13_unsupported_robustness` | 尚未实现的语法能干净地报错，不会崩溃 |
| `14_classes` | 构造/析构函数、私有成员访问控制、无拷贝语义、方法调用的借用检查、`this` |
| `15_function_overloading` | 按精确类型匹配解析重载、by-value/by-reference 独立轴、const/非-const 方法 |
| `16_namespaces` | 基本的 `namespace` 声明、限定调用、嵌套；`using namespace` 被拒绝 |

## 测试理念

- 每个 `.scpp` 文件都力求**严格符合 `docs/book/`**——如果某个测试失败了，先去
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
- **没有 `safe` 关键字**——每个函数默认都无条件被检查（ch01/ch08 Q13）；
  `unsafe { }` 是语言里唯一的安全上下文构造，且只放松 ch05 §5.5 里那一小串
  固定的操作（裸指针解引用、调用 `extern "C"` 函数等）外加 span 边界检查/
  溢出检查——所有权/移动/别名/生命周期检查（§5.1-§5.4）即便在 `unsafe { }`
  内部也无条件持续生效。调用 `extern "C"` 函数永远都需要 `unsafe { }`，跟
  调用方是谁无关，**也跟这个函数有没有函数体无关**——`extern "C"` 这个链接
  方式本身就标记了 FFI 边界（ch02 的边界表并没有区分这两种情况）；一个带函数
  体的 `extern "C"` 函数自己内部依然按普通函数一样被检查（例如它内部的裸指针
  解引用照样需要自己的 `unsafe { }`）。
- `docs/book/` 有时会落后于 `src/`（两者是独立维护的）——某个章节写着"尚未
  实现"，不代表现在依然如此（这一轮又证实了一次：尽管 ch06 的 backlog 仍把
  整个 ch11 标成"只是设计"，基本的 `namespace` 声明/限定查找/嵌套其实已经能
  用了，见 `16_namespaces`）。拿不准的时候，用 `scpp build` 快速探测一下就能
  确认。

## 现状

**123/123 通过**，在 `src/` 实现了 `safe` 关键字移除之后针对 `scpp` 验证过
（ch01/ch08 Q13：每个函数默认都无条件被检查；`unsafe { }` 是唯一的安全上下文
构造；文件后缀是 `.scpp`，不是 `.cpp`）。从纯文档驱动的重写到这一步，实际编译
之后又经过了两轮修正：
- `07_extern_c` 里有两个用例错误地以为带函数体的 `extern "C"` 函数不用
  `unsafe { }` 就能调用——ch02 的边界表并没有这个例外，所以调用处、以及
  `sum_point_by_pointer` 内部的裸指针解引用都补上了 `unsafe { }`。
- `namespace` 支持其实已经实现了（嵌套、限定调用），尽管 ch06 仍把它列在
  backlog 里——把原来那个方向错误的 `13_unsupported_robustness` 用例挪成了
  新分类 `16_namespaces` 下真正能通过的用例，并给确认还没实现的那一小块
  （`using foo::bar;` 单名导入）在 `13_unsupported_robustness` 里补了一个用例。

每次 `src/` 或 `docs/book/` 有后续变动后，重新跑一遍 `./build/run_tests`
来发现回归。
