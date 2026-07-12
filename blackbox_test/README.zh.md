# scpp 黑盒测试

> English version: [README.md](README.md)

这个目录是 `scpp` 编译器的**黑盒**测试套件。它与 `src/`（实现）、
`docs/book/`（面向读者的指南）以及 `docs/spec/`（形式化语言规范）相互独立地
维护：这里的测试完全通过阅读这些已发布文档来编写，并把编译好的 `scpp` CLI
当作外部工具来调用——就像语言的普通使用者一样，不依赖、也不需要了解 scpp
编译器的内部模块。

## 工作原理

- `cases/<NN_category>/<name>.scpp` —— 一段展示某条文档化语言规则的小 scpp
  程序（文件顶部注释会引用对应的 `docs/book/` 或 `docs/spec/` 章节）。scpp
  源文件用 `.scpp` 后缀，不用 `.cpp`（ch08 Q7/Q13）：既然现在每个函数默认都
  无条件被检查，就绝不能让一个普通的 `.cpp` 文件被悄悄喂给 scpp 编译器、在
  作者没有要求的情况下被检查。
- `cases/<NN_category>/<name>.expected` —— 如果 `scpp` 正确实现了规范，这段
  程序*应该*产生的结果。有三种形式：
  1. **第一行是一个数字**：`scpp` 必须编译成功，运行生成的可执行文件
     必须以这个退出码结束（0-255，遵循 POSIX `WEXITSTATUS`/shell `$?` 的语义
     ——被信号杀死的进程会被归一化为 `128+信号数`，例如 SIGABRT -> 134）。
     第一行之后的内容是期望的 stdout，逐字节比对。
  2. **`COMPILE_ERROR`**：`scpp` 必须以一个干净、为正数的退出码失败
     （一个真正的诊断信息，而不是崩溃）。具体的错误文案不做检查——规范并未
     锁定措辞。
  3. **`NO_ABORT`**：仅用于极少数场景——某个 scpp 插入的运行时检查（span
     边界检查、溢出检查）在一个 `[[scpp::unsafe]] { }` 块内被有意地*跳过*了（见
     ch01 §1.1），因此读到/算出的值本身就是不确定的垃圾值，没法固定下来
     断言——但进程仍必须正常终止（return/exit），而不是被信号杀死。
- **可选 CLI 用例辅助文件**：用于黑盒验证 CLI 表面本身：
  - `<name>.argv` / `main.argv` —— 每个非空行一个 argv token，支持
    `$INPUT`、`$OUTPUT`、`$TEMP` 占位符
  - `<name>.mode` / `main.mode` —— `command-only` 表示断言 CLI 命令
    自身的退出码/stdout，而不是去运行生成的可执行文件
  - `<name>.output` / `main.output` —— 输出文件在每个用例临时目录里的相对路径
    （默认 `case.bin`）；可用 `*`/`**` 通配目标 triple 相关的构建产物路径
  - `<name>.artifacts` / `main.artifacts` —— CLI 成功后必须存在的相对路径；
    若以前缀 `!` 开头，则表示该路径必须*不存在*
  - `<name>.stderr` / `main.stderr` —— CLI 命令期望的精确 stderr；`$TEMP`
    会展开成该用例的临时目录
- **多文件（ch11 模块）用例**：有些规则（跨文件的 import/export、
  partition……）确实需要不止一个源文件。一个包含 `main.scpp` 文件的目录会被
  当成*一个*模块测试用例，以该目录名命名：
  - `main.scpp` —— 入口文件，编译并运行方式和普通单文件用例完全一样；
    `main.expected` 是它的期望结果（形式同上面三种）。
  - `main.imports`（可选）——每个非空、非 `#` 注释行是一条
    `module_name=relative_path` 映射，会被转成 `scpp` 的
    `--import module_name=path`（ch11 §11.14）——把 `main.scpp` 需要的每个
    模块都列出来，不管是直接依赖还是间接依赖，因为只有 `main.scpp` 本身会
    被当作入口编译。
  - 目录里其它的 `.scpp` 文件——就是 `main.imports` 里引用的那些模块，不会
    被当成独立的用例扫描。
  - 若存在 `main.argv`，运行器会先把整个用例目录复制到临时工作区，再在那里
    调用 `scpp`；因此 project-build 类夹具可以安全地包含 `scpp.toml`、
    子包和嵌套源码树，而不会污染仓库里的已提交 fixture。

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
| `01_basics` | M1：标量、局部变量、`if`/`while`、函数、算术、零初始化、默认无条件检查，以及基础 `break`/`continue`、三目 `?:`、普通前向声明 |
| `02_structs` | `struct` 平凡性规则、零初始化、按位拷贝、禁止的成员类型 |
| `03_unique_ptr` | `std::make_unique`/`std::move`、移出检查、箭头语法糖 |
| `04_references_borrow` | `T&`/`const T&`、alias-XOR-mutability、NLL 借用释放、生命周期省略 |
| `05_span` | `std::span<T>` 的构造/下标/边界检查 |
| `06_unsafe_blocks` | `[[scpp::unsafe]] { }` 的门控与作用域规则；§5.1-§5.4 在其内部依然生效；函数级 `[[scpp::unsafe]]` 标记（ch01 §1.2，scpp 版的 `unsafe fn`） |
| `07_extern_c` | `extern "C"` 声明/定义、真实 libc 互操作 |
| `08_address_of` | `&expr`、`const T*`/`T*` 的区分 |
| `09_integer_overflow` | 默认检查并 abort、`[[scpp::unsafe]]` 下环绕、除法/取模的特殊情形 |
| `10_bool_and_char` | 标量间无隐式转换、逻辑运算符的短路求值 |
| `12_struct_vs_class` | `struct` 与 `class` 在访问控制上的分歧 |
| `13_unsupported_robustness` | 不支持/尚未实现的语法能干净地报错，不会崩溃 |
| `14_classes` | 构造/析构函数、私有成员访问控制、编译器提供/用户自定义的拷贝构造与拷贝赋值、只能由编译器提供的移动构造与移动赋值、方法调用的借用检查、`this` |
| `15_function_overloading` | 按精确类型匹配解析重载、by-value/by-reference 独立轴、const/非-const 方法 |
| `16_namespaces` | 基本的 `namespace` 声明、限定调用、嵌套、同一命名空间内类名的非限定查找，以及前缀 `::` 的全局作用域查找；`using namespace` 被拒绝 |
| `17_modules` | `export module`/`import`、命名空间与模块名匹配（ch11 §11.6）、跨模块 import/export/重新导出、裸 `extern`、partition |
| `18_closures` | lambda 表达式（ch05 §5.12）：按值/按引用/初始化捕获、笼统/混合捕获、引用捕获闭包的生命周期跟踪、显式 `this`/`*this` 捕获、`mutable`、尾置返回类型、泛型 lambda |
| `19_scalar_types` | `bool`/`int`/`char` 之外的完整标量家族（ch06）、标量间的显式转换，以及同类型/混合类型标量比较规则 |
| `20_generic_functions` | ch05 §5.11 的修订：完整 header 形式（裸/概念约束/多参数/仅返回类型）、缩写形式的裸 `auto`、概念约束的参数包 |
| `21_generic_types` | 泛型 `struct`/`class` 类型（ch05 §5.14）：裸/概念约束的类型参数、逐方法 `requires`、通过递归继承实现的 variadic 类型、非类型模板参数、基于基类推导的下标访问 |
| `22_lifetime_generic_parameters` | `[[scpp::lifetime(generic)]]`（ch05 §5.13）：预留的生命周期分组、闭包接受"被调用方选择的生命周期"时的调用点豁免 |
| `23_thread_safety_attributes` | `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`（ch05 §5.15）：结构化推导与手动覆盖 |
| `24_function_pointers` | 函数指针（ch05 §5.16）：真实 C/C++ 语法、unsafe-qualified/非-unsafe-qualified 的类型区分、取地址时的自动类型选择（普通函数 / `[[scpp::unsafe]]` / 无函数体 `extern "C"` / 有函数体 `extern "C"`）、单向转换、作为 struct 成员的合法性、可拷贝性、`&overloaded_name` 按目标类型解析 |
| `25_function_wrappers` | `std::function` / `std::move_only_function`（ch05 §5.18）：可拷贝/仅可移动 target、cv/ref-qualified 签名、moved-from 行为 |
| `26_threads` | `std::thread` / `std::jthread`：thread-movable 构造约束、join/detach/joinable 状态变化、`jthread` 析构时自动 join |
| `27_unions_packed_layout` | union 成员的 unsafe 门控，以及 `[[scpp::packed]]` 的布局/FFI 行为，包括 Linux `epoll_event` / `epoll_data_t` 形态 |
| `28_cli_invocation` | CLI 表面：直接 `scpp file.scpp` 构建、默认/自定义输出名、移除的 `build` 关键字，以及仍保留的 `lex`/`parse`/`build-module` 子命令 |
| `29_project_build` | manifest 驱动的项目构建：单包 `build`、workspace/path dependency、直接依赖可见性、`-p` 选包，以及对尚未实现 manifest 特性的拒绝路径 |
| `30_constant_evaluation` | 形式化规范驱动的 `constexpr`/`consteval` 覆盖：required constant evaluation、`if consteval` / `if !consteval`、v1 暂不支持的操作，以及“后面的参数先推导包，再回填前面依赖参数类型”的规则 |
| `31_enum_class` | scoped enumeration：`enum class` 声明、带作用域的枚举项访问、不同枚举类型分离、显式 cast，以及显式底层类型/枚举值 |
| `32_sizeof_storage_lifetime` | `sizeof(type)` / `sizeof(expr)`、`std::storage_for<T, ...>`、placement-new，以及显式析构调用语法 |
| `33_nodiscard` | 函数/类型上的 `[[nodiscard]]` / `[[nodiscard("reason")]]`，包括丢弃结果时报错，以及合法的非丢弃用法 |
| `34_expected_and_cstdlib` | `std::expected<T, E>` / `std::unexpected<E>` 的状态行为、误用时 abort，以及 `std::abort()` 本身 |

## 测试理念

- 每个 `.scpp` 文件都力求**严格符合已发布语言文档**（`docs/book/`，以及那些
  目前只先落在 `docs/spec/` 里的新工作）——如果某个测试失败了，先去查它引用
  的文档章节。如果测试本身其实违反了规范，就修正测试。如果测试确实符合规范
  却依然失败，那就是实现上的 bug，记录在这里留给 `src/` 的维护者去修——这个
  套件本身不会为了绕开失败而去改 `src/`。
- 程序优先通过**进程退出码**（`main` 的返回值）来观察行为；如果要验证真实的
  C 互操作，就使用通过 `extern "C"` 声明的真实 libc 调用（`puts`、
  `printf`）——这两者都是文档里明确记载的机制。`tests/test_source` 使用的那些
  仅供内部测试用的辅助函数（例如 `print_int`/`print_bool`/`print_char`）在这里
  被刻意**不予使用**，因为它们不属于文档记载的语言表面。
- 每个函数体（包括返回 `void` 的）都需要显式的 `return` 语句——scpp 目前没有
  隐式地"落到函数末尾就返回"这回事，尽管 `docs/book/` 里并没有明确点出这一点。
- **没有 `safe` 关键字**——每个函数默认都无条件被检查（ch01/ch08 Q13）；
  `[[scpp::unsafe]]` 是语言里唯一的安全上下文构造（一个 attribute，不是关键字
  ——见 ch01 §1.3，这一轮从裸的 `unsafe { }` 块重新设计成了
  `[[scpp::unsafe]] { }`），且只放松 ch05 §5.5 里那一小串固定的操作（裸指针
  解引用、调用 `extern "C"` 函数等）外加 span 边界检查/溢出检查——所有权/
  移动/别名/生命周期检查（§5.1-§5.4）即便在 `[[scpp::unsafe]] { }` 内部也
  无条件持续生效。调用 `extern "C"` 函数永远都需要 `[[scpp::unsafe]] { }`，
  跟调用方是谁无关，**也跟这个函数有没有函数体无关**——`extern "C"` 这个
  链接方式本身就标记了 FFI 边界（ch02 的边界表并没有区分这两种情况）；一个
  带函数体的 `extern "C"` 函数自己内部依然按普通函数一样被检查（例如它内部
  的裸指针解引用照样需要自己的 `[[scpp::unsafe]] { }`）。这一轮新增的机制：
  把 `[[scpp::unsafe]]` 直接标注在函数自己的声明上（返回类型之前），会让
  整个函数体变成 unsafe 上下文，*同时*让调用这个函数本身也变成 §5.5 的
  受控操作之一——相当于 scpp 版的 Rust `unsafe fn`（ch01 §1.2）。
- **`17_modules` 里 `--import name=path` 的具体行为现在已经验证过了**：
  `path` 确实直接指向该模块的原始 `.scpp` 接口源码，即时编译，不需要"先把
  模块编译成 `.scppm`"这一独立步骤——由 10 个通过的多文件用例实证确认。
  module partition 真正把多个文件合并成一个可导入模块这件事，在这个
  "每个模块名对应一条路径"的模型下依然没法表达——这里只覆盖了"外部文件
  不能直接 import 一个 partition"这条限制，没有覆盖"主接口单元汇聚
  partition"这个机制本身。
- **一个模块文件不能同时是可运行的程序**：一个包含 `export module name;`
  的文件，它的 `main()` 不会被链接成进程入口（通过一次"undefined
  reference to `main`"的链接错误实证发现的）。因此 `17_modules` 下每个
  多文件用例都用两个文件：一个普通（非模块）的 `main.scpp` 负责
  import 和调用，另一个独立的模块文件不带自己的 `main`。这是测试写法上
  的约束，不是文档记载的语言规则——`docs/book/` 里没有提到这一点，故在
  此标注。
- `docs/book/` 有时会落后于 `src/`（两者是独立维护的）——某个章节写着"尚未
  实现"，不代表现在依然如此（这一轮又证实了一次：基本的 `namespace` 声明/
  限定查找/嵌套其实已经能用了）。拿不准的时候，用 `scpp file.scpp` 之类的
  直接 CLI 调用快速探测一下就能确认。
- **`18_closures` 曾假设 `auto` 局部变量/返回类型推导已经能像真实 C++
  一样工作**，尽管 `docs/book/` 里从没有明确写过这一点已支持——**验证时
  确认这个假设是对的**，没有用例因为这个失败。
- **`18_closures` 里跟泛型函数/泛型 lambda 相关的用例，曾预期会依赖那个
  单独记录过的 generics/concepts 缺口**——结果只说对了一半：概念约束的
  泛型函数/lambda 现在已经实现了（`passing_closure_to_concept_constrained_generic_function.scpp`
  通过证实了这一点），但裸的（不带概念约束的）`auto` 参数是另一个独立的、
  依然存在的缺口（见下面的现状）——跟最初猜的"泛型完全没实现"不是一回事。

## 现状

当前维护中的基线：已用 CMake + Ninja 重新构建，并重新运行
`./build/run_tests`：

- **总共 306 个用例**
- 运行器原始统计 **306/306 通过**
- **`24_function_pointers`：14/14 都已得到有意义的验证**——解析器现已接受
  真正的函数指针声明，套件同时覆盖了正向运行路径和必须报 `COMPILE_ERROR`
  的安全规则
- **Move assignment 的旧目标值销毁** 现在覆盖了两种形态：
  - 一个持有 `std::unique_ptr` 的类：move assignment 期间先销毁目标对象原有状态，
    然后在作用域结束时再销毁替换后的新状态
  - 一个带用户自定义析构函数、手工管理资源（`strdup`/`free`）的类：验证旧目标状态
    会先被正确清理，再被 moved-in 的新状态覆盖
- **类按值传递/返回** 现在同时覆盖正向与反向场景：
  - 可拷贝类按值传参/返回
  - 仅可移动类通过 `std::move(...)` 按值传参/返回
  - 当语义上需要拷贝时，不可拷贝的裸局部变量依然会被拒绝
- **线程 trait override** 现在覆盖了重写后的 §5.15/§8 文档：
  内建 trait 谓词、泛型类上的条件 override、无条件泛型 override 的传播，以及
  `std::unique_ptr<T>` 的 trait 转发行为
- **函数/线程 wrapper** 现在也有直接黑盒覆盖：
  `std::function`、`std::move_only_function`、`std::thread`、`std::jthread`
  都有各自专门的用例目录来验证当前 stdlib 行为
- **这轮还补上了几类此前"太基础以至于没人单独写测试"的语言角落**：
  `break`/`continue`、三目 `?:`、普通前向声明、同一命名空间内未限定类名查找，
  以及混合标量类型比较必须被拒绝而不是崩溃
- **union / packed 布局现在也有直接黑盒覆盖**：
  union 成员访问的 unsafe 门控、packed struct 的原始字节布局，以及
  Linux `epoll_event` / `epoll_data_t` 的真实 FFI 声明形态
- **底层的 size/storage/lifetime 积木现在也有直接黑盒覆盖**：
  `sizeof(type)` / `sizeof(expr)`、`std::storage_for<T, ...>`、
  placement-new、显式析构调用，以及前缀 `::` 的全局作用域查找
- **`[[nodiscard]]` 现在也有直接黑盒覆盖**：
  函数级/类型级 nodiscard、带 reason 的诊断文本，以及那些本来就该
  继续被接受的“有消费结果”的正常用法
- **`std::expected` / `std::abort` 现在也有直接黑盒覆盖**：
  success/error 构造、对非默认可构造值的内联存储、误用时的 abort，
  以及直接调用 `std::abort()` 的进程终止路径
- **CLI 调用方式现在也有直接黑盒覆盖**：
  裸 `scpp file.scpp`、`-o custom_name`、被移除的 `build` 关键字拒绝路径，
  以及 `lex`、`parse`、`build-module` 子命令仍然可用
- **manifest 项目构建现在也有直接黑盒覆盖**：
  单包 lib/bin 构建、workspace/path dependency 构建、`-p` 选包、
  仅直接依赖可见的编译期规则，以及对 registry 依赖、
  `[workspace.dependencies]`、`[native]` 等延期特性的拒绝路径
