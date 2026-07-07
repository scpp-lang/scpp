# 3. 语法糖 / 既有语法的重新语义化

以下 C++ 写法被赋予强静态语义，**无条件、任何地方都生效**——包括在
`[[scpp::unsafe]] { }` 块内部（见 [§1.1](ch01-safety-context.md)）。只有
[§5.5](ch05-static-checks.md) 列出的那几个具体操作（裸指针解引用、
`new`/`delete`、调用 `extern "C"` 函数等等）才真正受 `[[scpp::unsafe]] { }`
控制；这张表里其余的东西不管在哪都照样生效。

| C++ 写法 | 语义 |
|----------|------|
| `std::move(x)` | 编译器内建 **move hint**。将 `x` 置为 *moved-out* 状态。此后读取 `x` 报错，直到重新赋值。不是普通函数调用。 |
| `T&` | 可变借用 `&mut T`：独占，参与别名 XOR 可变检查与生命周期检查。 |
| `const T&` | 共享借用 `&T`：可多个并存，但与任何 `&mut` 互斥。 |
| `T&&`（形参） | 按 move 传入（转移所有权）。 |
| `std::unique_ptr<T>` | 唯一所有权，由 `std` module 作为一个普通库 `class` 提供（`import std;`）。它的 move 语义之所以"天然契合"，不是编译器对它额外开特例，而是因为它本来就是一个遵循同一套 `class` 所有权规则的类型。 |
| `*x` / `x->y`（`x` 为 `std::unique_ptr<T>`，或者一个声明了 `operator*` 的 `class`） | 安全解引用/成员访问，得到指向对象的左值。对用户自定义 `class` 来说，`*x` 只是一次普通的 `x.operator*()` 调用语法糖，`x->y` 则只是 `(*x).y`——scpp 没有单独的 `operator->`。拥有者对象自己仍受别名 XOR 可变约束：对 `*x` 的借用记在 `x` 身上，借用期间移动（`std::move(x)`）或重新赋值 `x` 会报错（会造成悬垂/释放后使用）。见 [§5.17](ch05-static-checks.md#517-解引用运算符作用于-class)。 |
| `*p`（`p` 为裸指针 `T*`） | 需要 `[[scpp::unsafe]] { }`（见 [§1.3](ch01-safety-context.md)）。 |
| `&expr` | 取地址，`expr` 只能只读访问时得到 `const T*`，可变访问时得到 `T*`——这跟真实 C++ 自己的 `&expr` 规则一样。**在任何上下文里写它都合法**（不需要 `[[scpp::unsafe]] { }`）——被拦住的是解引用一个裸指针，不是造出一个裸指针，跟 Rust 的 `&x as *const T` 是一回事。`const T*`/`T*` 是真正不同的两个类型（只有单向隐式 `T* -> const T*` 转换）；通过 `const T*` 写是普通类型错误，无条件成立。见 [§5.7](ch05-static-checks.md)。 |
| `std::shared_ptr<T>` | 共享所有权（引用计数）。别名规则按内部可变性处理（v0.2 细化）。 |
| `std::span<T>` / `std::span<const T>` | 带生命周期检查的非拥有视图（"胖指针"：`{数据指针, 长度}`）。**v0.1 只能从定长数组构造**（`std::vector` 还不存在），构造后不能重新赋值（暂时按引用的规则处理：绑定一次，不可变更）。`.size` 读长度——**注意不是**真实 C++ 的 `.size()` 方法调用，scpp 还没有成员函数调用语法，这里当一个只读的计算字段处理。下标 `s[i]` 默认带运行时边界检查，越界调用 `abort()`（ch08 已定：v0.1 默认插入边界检查，panic 用 `abort()`）；在 `[[scpp::unsafe]] { }` 里则跳过，待遇跟整数溢出检查一样（见 [§1.1](ch01-safety-context.md)/[§5.8](ch05-static-checks.md)）。`std::string_view` 还没做（需要先有 `char` 类型）。 |
| 局部变量 `T x;` | 拥有其值；作用域结束时 drop（析构）。 |
| `new` / `delete` | 默认**禁止**；需 `[[scpp::unsafe]] { }`。（裸指针的**解引用**同样需要 `[[scpp::unsafe]] { }` 才能用，但 `T*` 这个**类型**本身、以及上面用 `&expr` 取一个裸地址，都不需要——见 [§5.5](ch05-static-checks.md)。） |
| `[[scpp::lifetime(name)]]` | Attribute（不是新关键字），把引用型形参/声明符分到具名的跨函数生命周期组——这是 scpp 相对 Rust `'a`/Circle `/a` 的可选退出式替代方案；见 [§5.3](ch05-static-checks.md)。 |
| `[capture-list](params) { body }`（lambda 表达式） | 跟真实 C++ 一样脱糖成一个匿名的、编译器合成的类：每个capture对应一个成员，`operator()` 实现函数体。按值capture是普通的拥有型成员；按引用capture是引用类型的成员，使闭包本身变成一个生命周期追踪的值。`this`/`*this` 必须显式capture——裸的 `[=]`/`[&]` 隐式capture `this` 是编译错误（真实 C++20 只是把它标记为 deprecated，P0806R2）。见 [§5.12](ch05-static-checks.md)。 |
| `extern "C" ...;` / `extern "C" ... { ... }` | 不是重新语义化，只是加限制：声明/定义一个 C 链接的函数，签名类型限定为 C-ABI 兼容类型。不带函数体的声明永远隐式不受检查（没东西可验证）——调用它需要 `[[scpp::unsafe]] { }`；带函数体的定义内部跟其它任何函数一样受检查。见 [§2.1](ch02-boundary-rules.md)。 |

**关键原则**：这些语义变化对用户是"隐形"的——他们写的还是熟悉的 C++，
只是默认到处都多了编译期报错来挡住 bug。

---

[← 上一章：边界规则](ch02-boundary-rules.md) · [目录](README.md) · [下一章：struct 与 class 的语义区分 →](ch04-struct-vs-class.md)
