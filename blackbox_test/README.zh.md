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
     边界检查、溢出检查）在一个 `[[scpp::unsafe]] { }` 块内被有意地*跳过*了（见
     ch01 §1.1），因此读到/算出的值本身就是不确定的垃圾值，没法固定下来
     断言——但进程仍必须正常终止（return/exit），而不是被信号杀死。
- **多文件（ch11 模块）用例**：有些规则（跨文件的 import/export、
  partition……）确实需要不止一个源文件。一个包含 `main.scpp` 文件的目录会被
  当成*一个*模块测试用例，以该目录名命名：
  - `main.scpp` —— 入口文件，编译并运行方式和普通单文件用例完全一样；
    `main.expected` 是它的期望结果（形式同上面三种）。
  - `main.imports`（可选）——每个非空、非 `#` 注释行是一条
    `module_name=relative_path` 映射，会被转成 `scpp build` 的
    `--import module_name=path`（ch11 §11.14）——把 `main.scpp` 需要的每个
    模块都列出来，不管是直接依赖还是间接依赖，因为只有 `main.scpp` 本身会
    被当作入口编译。
  - 目录里其它的 `.scpp` 文件——就是 `main.imports` 里引用的那些模块，不会
    被当成独立的用例扫描。

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
| `06_unsafe_blocks` | `[[scpp::unsafe]] { }` 的门控与作用域规则；§5.1-§5.4 在其内部依然生效；函数级 `[[scpp::unsafe]]` 标记（ch01 §1.2，scpp 版的 `unsafe fn`） |
| `07_extern_c` | `extern "C"` 声明/定义、真实 libc 互操作 |
| `08_address_of` | `&expr`、`const T*`/`T*` 的区分 |
| `09_integer_overflow` | 默认检查并 abort、`[[scpp::unsafe]]` 下环绕、除法/取模的特殊情形 |
| `10_bool_and_char` | 标量间无隐式转换、逻辑运算符的短路求值 |
| `12_struct_vs_class` | `struct` 与 `class` 在访问控制上的分歧 |
| `13_unsupported_robustness` | 不支持/尚未实现的语法能干净地报错，不会崩溃 |
| `14_classes` | 构造/析构函数、私有成员访问控制、无拷贝语义、方法调用的借用检查、`this` |
| `15_function_overloading` | 按精确类型匹配解析重载、by-value/by-reference 独立轴、const/非-const 方法 |
| `16_namespaces` | 基本的 `namespace` 声明、限定调用、嵌套；`using namespace` 被拒绝 |
| `17_modules` | `export module`/`import`、命名空间与模块名匹配（ch11 §11.6）、跨模块 import/export/重新导出、裸 `extern`、partition |
| `18_closures` | lambda 表达式（ch05 §5.12）：按值/按引用/初始化捕获、笼统/混合捕获、引用捕获闭包的生命周期跟踪、显式 `this`/`*this` 捕获、`mutable`、尾置返回类型、泛型 lambda |
| `19_scalar_types` | `bool`/`int`/`char` 之外的完整标量家族（ch06），以及标量间的显式转换 |
| `20_generic_functions` | ch05 §5.11 的修订：完整 header 形式（裸/概念约束/多参数/仅返回类型）、缩写形式的裸 `auto`、概念约束的参数包 |
| `21_generic_types` | 泛型 `struct`/`class` 类型（ch05 §5.14）：裸/概念约束的类型参数、逐方法 `requires`、通过递归继承实现的 variadic 类型、非类型模板参数、基于基类推导的下标访问 |
| `22_lifetime_generic_parameters` | `[[scpp::lifetime(generic)]]`（ch05 §5.13）：预留的生命周期分组、闭包接受"被调用方选择的生命周期"时的调用点豁免 |
| `23_thread_safety_attributes` | `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`（ch05 §5.15）：结构化推导与手动覆盖 |

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
  限定查找/嵌套其实已经能用了）。拿不准的时候，用 `scpp build` 快速探测一下
  就能确认。
- **`18_closures` 曾假设 `auto` 局部变量/返回类型推导已经能像真实 C++
  一样工作**，尽管 `docs/book/` 里从没有明确写过这一点已支持——**验证时
  确认这个假设是对的**，没有用例因为这个失败。
- **`18_closures` 里跟泛型函数/泛型 lambda 相关的用例，曾预期会依赖那个
  单独记录过的 generics/concepts 缺口**——结果只说对了一半：概念约束的
  泛型函数/lambda 现在已经实现了（`passing_closure_to_concept_constrained_generic_function.scpp`
  通过证实了这一点），但裸的（不带概念约束的）`auto` 参数是另一个独立的、
  依然存在的缺口（见下面的现状）——跟最初猜的"泛型完全没实现"不是一回事。

## 现状

**总共 197 个用例，179 个通过。** `01_basics`-`17_modules` 里的 137 个，
加上 `18_closures` 里的 17/20 个，都是之前已经验证过的（两轮的详细内容见
下文）。这一轮是在文档和 `src/` 同时发生一次大更新之后重新做的完整验证：
`unsafe { }` 被重新设计成了 `[[scpp::unsafe]]` attribute（ch01 §1.1-1.3，
既能标在一个块上，也能标在函数声明上——scpp 版的 `unsafe fn`），`class`
成员变量现在可以是 `public`（ch04 §4.2，推翻了之前"成员变量必须私有"的
规则），ch05 里新增了四大块内容：泛型函数修订（§5.11：裸/完整 header 形式/
多参数/仅返回类型）、闭包（§5.12，之前已验证过）、生命周期泛型参数
（§5.13）、泛型类型（§5.14）、线程安全结构化属性（§5.15）。所有现有的
`.scpp` 文件都从 `unsafe { }` 批量改成了 `[[scpp::unsafe]] { }`，另外
扩充了 2 个已有分类（`06_unsafe_blocks`、`12_struct_vs_class`），新增了
5 个全新分类（`19_scalar_types` 到 `23_thread_safety_attributes`）。

**这一轮迄今为止最重要的发现**：绝大部分标量类型现在根本没法用，而且
标量间的显式转换完全不工作（`19_scalar_types`，8/8 全部失败）——
`int8_t`/`int16_t`/`int32_t`/`int64_t`、对应的 `uint*_t`、`long`、
`unsigned int`、`unsigned long`、`float32_t`/`float64_t`/`float`/
`double`、`size_t`、`ptrdiff_t` **全部**都没法作为类型名解析（目前只有
`bool`、`int`、`char` 能用），`static_cast<T>(expr)` 和 C 风格的
`(T)expr` 转换也都不接受，哪怕是在两个本身都能正常工作的标量类型
（`bool`/`int`）之间转换也不行。这是在探测泛型的非类型模板参数时偶然
发现的——`docs/book/` 里没有任何地方暗示这是已知缺口（ch06 的标量表把
整个家族都当作无条件可用的，没有任何"尚未实现"的标注）。

总共 18 个已知失败，都是真正的实现发现（测试本身合规，没有改动；闭包那
一轮的发现这里只简要总结，完整内容见 git 历史）：

- **`19_scalar_types`**（8/8 失败）：见上文。
- **`20_generic_functions`**（4/6 通过）：完整 header 形式（裸类型参数、
  概念约束、多类型参数、仅返回类型）全都工作正常。有两种文档里写明的形式
  还没法解析：缩写形式的裸 `auto` 参数（`int f(auto x)`——已确认不是
  lambda 专属的，跟 `18_closures` 发现的是同一个缺口），以及概念约束的
  参数包（`const Concept auto&... args`，报错"expected parameter name
  but found '...'"）。
- **`21_generic_types`**（7/9 通过）：裸类型参数、逐方法 `requires`
  子句、泛型 struct 的概念约束强制检查、通过递归继承实现的 variadic
  类型（`Tuple`/`TupleImpl` 那两种模式）、非类型参数配合类型参数包，
  全都工作正常。两个发现：（1）裸（不受约束）的泛型类型参数错误地允许
  在它上面调用方法——比如 `Holder<T>` 内部的 `this->item.doubled()`
  能编译通过，尽管 `T` 在那里完全没有约束，这跟 §5.11/§5.14 都确立的
  "一次性在定义处检查"原则矛盾（要等到调用方用不兼容的类型实例化时才会
  报错，而不该是这样）；（2）只带一个非类型参数、没有类型参数的泛型类型
  （比如 `template<int N> class X`）没法实例化（"expected a type
  name"），尽管同一个机制配合类型参数包时是能正常工作的。另外记录了一个
  非新增的旧发现：一般（非泛型、非 variadic）的 class 继承按理说还应该是
  推迟支持的（ch04 §4.2）——它在声明处并没有被干净地拒绝，但用起来会对一
  个继承来的、原本是 `public` 的字段报出令人困惑的"cannot access
  private member"，既不是干净的拒绝，也不是真正能用的继承。
- **`22_lifetime_generic_parameters`**（3/3 通过）：`[[scpp::lifetime
  (generic)]]` 标签在普通函数参数上工作正常，"被调用方内部现造一个值、
  再拿它去调用传进来的闭包"这个核心模式，在概念只需要检查"可调用"的时候
  也能正常工作。有一种文档里写的形式没法解析：给 `requires` 表达式自己的
  探测参数打上同样的 attribute（"expected ')' but found '['"）。
- **`23_thread_safety_attributes`**（6/6 通过）：测试到的每一条结构化
  推导规则（标量类型、按引用捕获 vs 按值捕获的闭包、带不带手动覆盖的裸
  指针字段）都跟文档描述完全一致。
- **`18_closures`**（17/20 通过，上一轮的发现）：按引用捕获的闭包，其
  借用没有像 NLL 承诺的那样在最后一次使用后释放（对比 `std::span`，
  它是能正确释放的）；`[*this]` 被"class 还没有拷贝语义"这个早就存在的
  缺口挡住了；lambda 的裸 `auto` 参数没法解析（跟上面
  `20_generic_functions` 的发现是同一个根本原因）。
- **`12_struct_vs_class`**（3/4 通过，这一轮新增）：public 成员变量的
  普通读写工作正常，但对同一个对象，一个 public 字段的活跃借用**没有**
  正确地跟调用一个会修改状态的方法产生冲突——尽管 whole-root-conservative
  的字段对字段借用冲突（两个不同的 public 字段）是能正常工作的——ch04
  §4.2 描述的那种组合方式（"两者最终都会针对同一个根对象记录一次借用"）
  在字段路径和方法调用路径之间还没有完全接通。
- **`04_references_borrow` 里的 2 个发现**，跟任何具体的新章节都无关，
  是在构造其它测试时顺带发现的：`const T&` 参数没法绑定到一个临时值/
  字面量参数上（只能绑定到一个具名的左值——`f(42)` 会失败，而对一个局部
  变量 `int v` 调用 `f(v)` 是可以的），以及一个普通的 `const` 局部变量
  声明（`const int x = 5;`）会被直接拒绝（"'const' is only supported
  directly before a reference type or a pointer type"）。
- **`06_unsafe_blocks` 里的 1 个发现**：对指针运算的结果做解引用
  （`*(p + 1)`）还不支持——目前只支持对一个裸的局部变量/字段直接解引用，
  比 ch05 §5.5 里把"对指针类型的值做间接寻址，或者做指针运算"列为同一条
  受控操作要窄。

`01_basics`-`17_modules`（前几轮验证过的分类）目前没有已知失败。

`import ... as`（模块别名）**不是** scpp 的特性——它曾短暂地出现在
ch11 §11.8 里，但后来确认这根本不是真实的 C++20 语法（对照 cppreference
核实过：标准只有 `import name;` 和 `import name:part;`），已作为文档
bug 被移除（`0413530`）。即便这已经不是文档规则了，验证 scpp 对这段语法
是干净地报错、而不是崩溃或者误解析，依然是有价值的——毕竟这正是一个
Python/Rust 程序员很可能下意识去试的写法——所以
`13_unsupported_robustness/import_as_aliasing_is_rejected_not_crashed.scpp`
保留了下来（改成验证"不支持/不存在的语法必须干净地报错"，而不是"文档
定义了但还没实现"）。

上一轮验证时的修正，留作参考：
- `07_extern_c` 里有两个用例错误地以为带函数体的 `extern "C"` 函数不用
  `unsafe { }` 就能调用——ch02 的边界表并没有这个例外，所以调用处、以及
  `sum_point_by_pointer` 内部的裸指针解引用都补上了 `unsafe { }`。
- `namespace` 支持其实当时已经实现了（嵌套、限定调用），尽管 ch06 当时仍把它
  列在 backlog 里——把原来那个方向错误的 `13_unsupported_robustness` 用例挪成
  了新分类 `16_namespaces` 下真正能通过的用例，并给确认还没实现的那一小块
  （`using foo::bar;` 单名导入）在 `13_unsupported_robustness` 里补了一个用例。
- 有 4 个 `17_modules` 用例最初被写成单文件形式，把 `export module X;`
  和一个顶层可运行的 `main()` 放在了一起——这样悄悄地就没法链接了（见上面
  "模块文件不能同时是可运行程序"这条说明）。把每一个都拆成了"模块 + 普通
  调用方"两个文件。
- `17_modules/export_import_re_exports_transitively` 最初链接失败，报错
  "undefined reference to `_scppM1_bF5_valueP0_`"——一个透传重导出的符号，
  它被 mangle 出来的名字错误地用了做重导出的模块（`b`）而不是真正定义它的
  模块（`a`）作为前缀。已经过实证隔离（换成*间接*调用同一个函数——让
  `b::helper()` 内部转调 `a::value()`——完全正常，从而把 bug 定位到"调用点
  仅通过透传重导出链才能到达的符号"这个具体场景）。已报告给 `src/`；
  重新验证时确认已修复。


