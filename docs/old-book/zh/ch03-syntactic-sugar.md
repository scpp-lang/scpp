# 3. 熟悉语法背后的借用、移动与视图

到目前为止，scpp 看起来还像一门“小而熟悉、长得像 C++”的语言。这一章开始进入
它真正特别的地方：**很多表面完全不变的 C++ 写法，在 scpp 里会获得更强的静态语义。**

这正是 scpp 想做的事：不逼你先学一整套全新表面语言，也尽量拿到 Rust 式的安全保
证。你写的仍然是 `T&`、`const T&`、`std::move(x)`、`std::unique_ptr<T>`、
`std::span<T>`；变化在于，编译器不再把这些东西只当成“程序员约定”，而是把它们
纳入一套受检查的所有权模型。

## 3.1 `T&` 和 `const T&` 是借用

在普通 C++ 里，引用常常给人的感觉只是“另一个名字”。但在 scpp 里，它们更精确：

- `T&` 是**可变借用**；
- `const T&` 是**共享借用**。

也就是说，编译器会追踪：这段时间里谁可以读，谁可以写。

```cpp
extern "C" int puts(const char* s);

void add_bonus(int& score) {
    score = score + 1;
    return;
}

int doubled(const int& score) {
    return score + score;
}

int main() {
    int score = 20;
    add_bonus(score);
    int total = doubled(score);

    [[scpp::unsafe]] {
        if (score == 21 && total == 42) {
            puts("references are checked borrows");
        }
    }
    return 0;
}
```

输出：

```text
references are checked borrows
```

真正值得记住的不是“它能工作”，而是**它为什么能工作**：

- `add_bonus` 通过 `int&` 临时拿到独占写权限；
- `doubled` 通过 `const int&` 只读地借用这个值；
- 每次借用结束后，调用者就重新拿回对 `score` 的普通使用权。

第 5 章会把完整的别名规则讲清楚。现在先抓住这个心智模型：**scpp 里的引用是借用
检查器的一部分，不是随手起的别名。**

## 3.2 `std::move` 真的表示“所有权已经搬走了”

在 C++ 里，`std::move` 常被解释成“一个触发 move 的 cast”。但在 scpp 里，这个说
法不够强。这里的 `std::move(x)` 更接近于：

> “把所有权从 `x` 里转移出去；在重新给 `x` 赋新值之前，不允许我再读取它。”

```cpp
import std;
extern "C" int puts(const char* s);

int main() {
    std::unique_ptr<int> first = std::make_unique<int>(7);
    std::unique_ptr<int> second = std::move(first);

    if (*second == 7) {
        [[scpp::unsafe]] {
            puts("moves leave the old name behind");
        }
    }
    return 0;
}
```

输出：

```text
moves leave the old name behind
```

move 之后，`second` 成了新的拥有者，`first` 则进入 moved-out 状态；如果后面再读
`first`，会直接变成编译错误。

这正是 scpp “重新语义化熟悉语法”的关键例子之一：写出来还是 `std::move`，但编译器
把它当作一次受检查的所有权转移。

## 3.3 在拥有型指针上，`*` 和 `->` 依然顺手

scpp 不想让安全代码变得别扭。只要你是通过 `std::unique_ptr<T>` 这样的拥有型工具来
持有对象，解引用和成员访问仍然保留普通“指针味”的写法。

```cpp
import std;
extern "C" int puts(const char* s);

struct Pair {
    int left;
    int right;
};

int main() {
    std::unique_ptr<Pair> pair = std::make_unique<Pair>();
    pair->left = 10;
    pair->right = 32;

    [[scpp::unsafe]] {
        if (pair->left + pair->right == 42) {
            puts("unique_ptr still feels like a pointer");
        }
    }
    return 0;
}
```

输出：

```text
unique_ptr still feels like a pointer
```

表面用起来很轻松，但语义比裸指针安全得多：

- `pair` 始终只有一个清晰的拥有者；
- 对 `*pair` 的借用会记在 `pair` 自己身上；
- 如果借用还活着，就不能把 `pair` move 走，也不能重新赋值。

也就是说，语法保留熟悉感，所有权故事依然由编译器维持一致。

## 3.4 `std::span<T>` 是借来的视图

`std::span<T>` 和 `std::span<const T>` 是 scpp 里表达“看看一段已有序列，但不拥有
它”的方式。

在 v0.1 里，最重要的心智模型是：span 是建立在**已有定长数组**之上的受检查视图。

```cpp
import std;
extern "C" int puts(const char* s);

int sum(std::span<const int> values) {
    int i = 0;
    int total = 0;
    while (i < values.size) {
        total = total + values[i];
        i = i + 1;
    }
    return total;
}

int main() {
    int numbers[4];
    numbers[0] = 5;
    numbers[1] = 10;
    numbers[2] = 12;
    numbers[3] = 15;

    std::span<const int> view = numbers;

    [[scpp::unsafe]] {
        if (sum(view) == 42) {
            puts("span borrows an existing array");
        }
    }
    return 0;
}
```

输出：

```text
span borrows an existing array
```

这里有三个值得顺手记住的细节：

1. `view` **不拥有** `numbers`；
2. `view.size` 是像字段一样读取长度；
3. 下标访问默认带边界检查。

因此，`std::span` 就是 scpp 里最自然的“slice 风格、非拥有型视图”工具。

## 3.5 裸指针依然存在，但“相信它”必须显式写出来

scpp 并没有把裸指针从语言里删掉；它做的是把信任边界标出来。

用 `&value` 产生一个裸指针，在安全代码里是允许的；但一旦你准备真正解引用它，就必
须进入 `[[scpp::unsafe]]`，并在那个局部位置承担责任。

```cpp
extern "C" int puts(const char* s);

int main() {
    int value = 41;
    int* p = &value;

    [[scpp::unsafe]] {
        *p = *p + 1;
        if (value == 42) {
            puts("raw pointers need an explicit trust boundary");
        }
    }
    return 0;
}
```

输出：

```text
raw pointers need an explicit trust boundary
```

这就是后面整本书都会反复出现的模式：

- 拥有型值、借用、span 默认都能在受检查代码里工作；
- 裸指针并没有被禁止；
- 但真正去相信一块未经证明的内存时，那个点必须被显式写出来。

## 3.6 怎样理解“熟悉语法被重新语义化”

如果你想要一句最短总结，可以直接记这几条：

- `T&` 表示独占借用；
- `const T&` 表示共享借用；
- `std::move(x)` 表示所有权转移；
- `std::unique_ptr<T>` 很自然地落在这套所有权规则里；
- `std::span<T>` 是非拥有型视图；
- 裸指针是真实存在的，但解引用必须显式进入 unsafe。

这就是 scpp 的核心技巧：表面继续保持 C++ 风格，编译器则在背后把某些熟悉写法解释
成更强的静态语义。

## 重写过程中保留的参考附录

下面这部分保留了本章原先“精确映射表”的价值，同时让书的前半部分可以逐步改写成教
程式阅读路径。

这些语义变化在**任何地方都无条件生效**——包括 `[[scpp::unsafe]] { }` 内部。真正
受 `[[scpp::unsafe]] { }` 控制的，只有 [§5.5](ch05-static-checks.md) 那份固定操
作清单。

| C++ 写法 | 语义 |
|----------|------|
| `std::move(x)` | 编译器内建 **move hint**。将 `x` 置为 *moved-out* 状态。此后读取 `x` 报错，直到重新赋值。不是普通函数调用。 |
| `T&` | 可变借用 `&mut T`：独占，参与别名 XOR 可变检查与生命周期检查。 |
| `const T&` | 共享借用 `&T`：可多个并存，但与任何 `&mut` 互斥。 |
| `T&&`（形参） | 按 move 传入（转移所有权）。 |
| `std::unique_ptr<T>` | 唯一所有权，由 `std` module 作为一个普通库 `class` 提供（`import std;`）。它的 move 语义之所以天然契合，是因为它本来就是一个遵循同一套 `class` 所有权规则的类型。 |
| `*x` / `x->y`（`x` 为 `std::unique_ptr<T>`，或者一个声明了 `operator*` 的 `class`） | 安全解引用 / 成员访问，得到指向对象的左值。对用户自定义 `class` 来说，`*x` 只是普通 `x.operator*()` 调用的语法糖，`x->y` 则只是 `(*x).y`——scpp 没有单独的 `operator->`。拥有者对象自己仍受别名 XOR 可变约束：对 `*x` 的借用记在 `x` 身上，因此借用存活期间移动（`std::move(x)`) 或重新赋值 `x` 都会报错。见 [§5.17](ch05-static-checks.md#517-解引用运算符作用于-class)。 |
| `*p`（`p` 为裸指针 `T*`） | 需要 `[[scpp::unsafe]] { }`（见 [§1.3](ch01-safety-context.md)）。 |
| `&expr` | 取地址，若 `expr` 只能只读访问则得到 `const T*`，若可变访问则得到 `T*`——和真实 C++ 的 `&expr` 规则一致。它在任何上下文里都可以写；受管制的是“解引用裸指针”，不是“制造裸指针”。`const T*`/`T*` 是真正不同的类型（只有单向隐式 `T* -> const T*`）。见 [§5.7](ch05-static-checks.md)。 |
| `std::shared_ptr<T>` | 共享所有权（引用计数）。别名规则按内部可变性处理（v0.2 细化）。 |
| `std::span<T>` / `std::span<const T>` | 带生命周期检查的非拥有视图（“胖指针”：`{数据指针, 长度}`）。v0.1 只能从定长数组构造，构造后也不能重新赋值。`.size` 把长度暴露成一个计算字段，而不是成员函数调用。`s[i]` 默认带运行时边界检查，越界时调用 `abort()`；在 `[[scpp::unsafe]] { }` 中则跳过。`std::string_view` 目前还不存在。 |
| 局部变量 `T x;` | 拥有其值；作用域结束时 drop（析构）。 |
| `new` / `delete` | 默认禁止；需 `[[scpp::unsafe]] { }`。 |
| `[[scpp::lifetime(name)]]` | Attribute（不是新关键字），把引用型形参 / 声明符分到具名的跨函数生命周期组——这是 scpp 相对 Rust `'a` / Circle `/a` 的可选退出式替代方案；见 [§5.3](ch05-static-checks.md)。 |
| `[capture-list](params) { body }`（lambda 表达式） | 跟真实 C++ 一样脱糖成匿名、由编译器合成的类：每个 capture 对应一个成员，`operator()` 实现函数体。按值 capture 是普通拥有型成员；按引用 capture 是引用类型成员，因此闭包本身也会进入生命周期追踪。`this` / `*this` 必须显式 capture。见 [§5.12](ch05-static-checks.md)。 |
| `extern "C" ...;` / `extern "C" ... { ... }` | 不是重新语义化，只是加限制：声明 / 定义一个 C linkage 函数，签名类型必须是 C ABI 兼容类型。不带函数体的声明永远隐式不受检查，因此调用它需要 `[[scpp::unsafe]] { }`；带函数体的定义内部则和其它普通函数一样受检查。见 [§2.1](ch02-boundary-rules.md)。 |

---

[← 上一章：基本构件](ch02-boundary-rules.md) · [目录](README.md) · [下一章：struct 与 class →](ch04-struct-vs-class.md)
