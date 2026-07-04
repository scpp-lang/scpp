# 2. 边界规则（Safe ↔ Unsafe 交互）

这是健全性的关键，必须严格。

| 调用方向 | 规则 |
|----------|------|
| `unsafe` 调 `safe` | **自由放行**。safe 函数对任何调用者都安全。 |
| `safe` 调 `safe` | 自由放行，正常参与检查。 |
| `safe` 调 `unsafe` | **必须包在 `unsafe { }` 内**，否则编译错误。程序员以此背书。 |
| `safe` 内解裸指针 | 必须在 `unsafe { }` 内。 |

- 边界处的数据契约：safe 函数暴露给 unsafe 世界的引用/指针，其生命周期
  义务对 unsafe 侧**不强制**（unsafe 侧自负）。反之，unsafe 传入 safe 的
  引用，safe 侧**假定其在函数调用期间有效**（调用者义务）。
- 编译器需能标记一个 `unsafe` 函数是否"已人工审核为可安全调用"——v0.1
  不做形式化，先靠 `unsafe { }` 背书。
- 机制：具体规则见 [§1.3](ch01-safety-context.md)（`unsafe { }`，设计已
  定稿，尚未实现）。简单说：检查器会拒绝任何被调方 `Function::is_safe`
  为 false 的 `Call`，除非调用点在词法上位于 `unsafe { }` 块内（或者调用者
  自己就是 `unsafe` 函数）——同一个"当前是否在 unsafe 里"标记，也会用来
  放行裸指针解引用，以及以后 [§5.5](ch05-static-checks.md) 里其余各项
  语法落地后的放行。

## 2.1 `extern "C"` 声明（设计已定稿，尚未实现）

这是跟**真正的** C 打交道的边界，不只是跟不受检查的 scpp 代码打交道——
调用 libc 或任何其他 C 库。这里完全复用 C++ 现有语法，scpp 不加任何新写
法，只加额外限制和下面这套安全接线。

- **语法**：跟 C++ 现有形式完全一样——
  ```cpp
  extern "C" int printf(const char* fmt, ...);   // 单条声明
  extern "C" {                                    // 块形式：等价于给每条
      void* malloc(size_t size);                  // 声明都重复写一遍
      void free(void* p);                          // `extern "C"`，
      void abort();                                // 跟真实 C++ 一样
  }
  ```
  v0.1 只接受字面量链接字符串 `"C"`（不支持 `"C++"` 或别的）——写别的
  字符串是编译错误，报错里会说明目前只支持哪个。
- **声明 vs 定义——两者行为不一样**：
  - **没有函数体**（`extern "C" int foo(int x);`）：声明一个*在别处定义*、
    靠外部链接进来的函数。编译器看不到它的实现，所以**永远隐式是
    `unsafe`**——写 `safe extern "C" int foo(int x);` 是编译错误
    （"不能把一个外部声明标成 safe：编译器看不到它的实现"）。从 `safe`
    函数调用它因此需要 `unsafe { }`，走的是和其他 safe 调 unsafe 完全
    一样的机制（不需要任何新规则——这正是这一节的要点：`extern "C"` 只是
    新增了一个"天生就是 unsafe"的函数签名**来源**，完全骑在
    [§1.3](ch01-safety-context.md) 已经定义好的机制上）。
  - **有函数体**（`extern "C" int add(int a, int b) { return a + b; }`）：
    定义一个普通的 scpp 函数，只是额外获得 C 链接，好让外部 C（或其他
    语言）代码能调用它。这里 `safe` 和 `extern "C"` 是**正交**的——
    `safe extern "C" int add(...) { ... }` 是允许的，函数体和其他 `safe`
    函数一样被完整检查；`extern "C"` 只约束**签名**的类型（见下）并请求
    C 链接，不代表函数体本身可信与否。这跟 Rust 的
    `#[no_mangle] pub extern "C" fn foo(...)` 是一回事——签名必须
    FFI-safe，函数体照样是被正常检查的 Rust。
- **签名类型限定为 C-ABI 兼容类型**，声明和定义两种形式的每个参数和返回
  类型都要检查：标量；裸指针 `T*`（包括 `void*`——`void` 在这里成为一个
  合法的、仅用作指针指向类型的类型名；`const T*` 是独立的类型，见
  [§5.7](ch05-static-checks.md)——上面 `printf` 的 `const char* fmt`
  现在是真的只读了，不是被丢弃的限定符）；`struct`（本来就保证是
  Clang-ABI 兼容布局，见 [§4.3](ch04-struct-vs-class.md)），按值或按指针
  均可；形参位置的定长数组 `T[N]`（退化为指针，和普通 C++ 一样）。
  **被拒绝的**：`T&`/`const T&`、`std::unique_ptr`、`std::span`、
  `std::string`/`std::string_view`、`std::vector`、`std::shared_ptr`、
  `std::expected`（见 [§5.6](ch05-static-checks.md)/
  [§6](ch06-safe-subset.md)——可恢复错误同样没有对应的 C 表示），
  以及 `[[scpp::lifetime(name)]]`（没有借用检查类型可以附着，没有意义）
  ——这些都没有对应的 C 表示。一个 `safe extern "C"` 函数如果内部需要用
  scpp 的所有权/借用类型，就在边界上用 C 兼容的原始形式收发，进函数体后
  自己（受检查地）转换。
- **这个功能还缺的前置条件**（都不是 `extern "C"` 专属的——只是它恰好是
  第一个用到这些的功能，属于通用缺口）：
  - `extern` 关键字（还没做词法支持）。
  - 最小限度的字符串字面量词法支持，够认出 `"C"` 这一个 token 就行——
    v0.1 还不需要把字符串字面量做成通用表达式类型（那要等
    `std::string`/`char`，见 [§6](ch06-safe-subset.md)）；这里只是那项
    未来工作里一个很窄、独立的切片。
  - `void` 作为合法类型名，用于 `void*` 形参/局部变量和返回 `void` 的
    函数——scpp 现在完全没办法声明一个返回 `void` 的函数
    （`to_llvm_type` 没有处理它的分支）。
  - 变长参数（`...`，给 `printf` 系列函数用）有用，但**不是**第一版
    的硬性要求：大部分常用的 libc 入口（`malloc`、`free`、`memcpy`、
    `strlen`……）都不是变长参数的。可以先在声明里解析并存一个
    `has_varargs` 标记，真正的变长调用点 codegen（参数提升、
    `llvm::FunctionType` 上的 `isVarArg=true`）留作后续快速跟进。
- **实现形状**：这其实是把 codegen.cppm 里已经手写了三遍的模式泛化——
  `get_or_declare_malloc`/`get_or_declare_free`/`get_or_declare_abort`
  各自手动搭一个 LLVM `FunctionType`，用 `ExternalLinkage`、不带函数体
  `Function::Create` 出来，对应一个写死的 libc 函数。用户写的
  `extern "C"` 声明应该从解析出来的签名，通用地生成同样形状的 LLVM
  `declare`，而不是每加一个新的 libc 依赖就得手写一个新的 C++ 方法。

---

[← 上一章：安全上下文](ch01-safety-context.md) · [目录](README.md) · [下一章：语法糖 →](ch03-syntactic-sugar.md)
