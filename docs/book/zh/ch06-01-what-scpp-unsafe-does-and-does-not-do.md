# `[[scpp::unsafe]]` 会做什么、不会做什么

scpp 的设计目标，是让绝大多数代码都留在语言受检查的普通区域里。不过有时候，程
序确实需要做一些编译器无法自行证明安全的事，例如解引用裸指针，或者调用一个未
受检查的外部函数。

这就是 `[[scpp::unsafe]]` 的用途。

同样重要的是，`[[scpp::unsafe]]` 被刻意设计得很窄。它**不会**关闭所有权检查、
借用检查或生命周期检查。它只会打开少数几个需要程序员在局部自己承担理由的安全
边界。

下面每个可运行示例都可以保存成 `unsafe.scpp`，然后这样构建并运行：

```sh
scpp unsafe.scpp -o unsafe
./unsafe
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输
出，请把文件保存成诊断块里显示的那个描述性文件名。

## 使用 unsafe block

最常见的形式，是一个 unsafe block。block 外面的代码仍然是普通的安全 scpp 代
码。

```cpp
import std;

int read_value(int* pointer) {
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    int value{42};
    std::println("{}", read_value(&value));
    return 0;
}
```

输出：

```text
42
```

像 `&value` 这样形成裸指针，在安全代码里本来就是允许的。真正的 unsafe 边界出
现在程序决定“信任这个指针并解引用它”的那一刻。

## 同样的操作在 `[[scpp::unsafe]]` 外会被拒绝

如果你试图在普通安全代码里解引用这个裸指针，编译器就会拦下你。

```cpp
int read_value(int* pointer) {
    return *pointer;
}

int main() {
    int value{42};
    return read_value(&value);
}
```

编译器输出：

```text
raw_pointer_unsafe_fail.scpp:2:12: error: cannot dereference raw pointer 'pointer': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)
 2 |     return *pointer;
   |            ^
```

所以，`[[scpp::unsafe]]` 不是风格提示。它是真正决定某些操作是否构成良构程序的
门槛。

## 你也可以把整个函数标成 unsafe

如果某个辅助函数的全部意义，就是执行一种 unsafe 操作，那么你也可以直接把属性
写在函数上。

```cpp
import std;

[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    [[scpp::unsafe]] {
        std::println("{}", read_first(&value));
    }
    return 0;
}
```

输出：

```text
9
```

这表示 `read_first` 的整个函数体都处在 unsafe context 里。但这**不**表示调用者
就自动安全了。

## 调用 unsafe 函数本身也需要 unsafe context

被标记为 unsafe 的函数，自带一个未受检查的前提条件，因此调用点也必须明确承认
这一点。

```cpp
[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    return read_first(&value);
}
```

编译器输出：

```text
call_unsafe_function_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

这正是这套设计的核心思想：unsafe 假设应当留在代码真正依赖它们的那个位置上，保
持可见。

## `[[scpp::unsafe]]` 不会关闭借用检查

unsafe 代码仍然会继续受到所有权与别名规则的检查。

```cpp
int main() {
    int value{5};
    [[scpp::unsafe]] {
        int& first = value;
        int& second = value;
        return first + second;
    }
}
```

编译器输出：

```text
unsafe_still_checks_borrows_fail.scpp:5:9: error: cannot mutably borrow 'value': it is already borrowed
 5 |         int& second = value;
   |         ^
```

所以，`[[scpp::unsafe]]` **并不**等于“把检查器关掉”。它的意思只是“在这里允许语
言中那些被明确设门的操作之一”。

## `[[scpp::unsafe]]` 主要是拿来做什么的

从高层看，`[[scpp::unsafe]]` 是 scpp 允许你跨越少数几个明确边界的位置，例如：

- 解引用裸指针，或者做指针算术；
- 调用没有函数体的 `extern "C"` 函数；
- 访问 union 成员；
- 使用原始的 `new` 或 `delete`；
- 调用那些自身声明就带有 `[[scpp::unsafe]]` 的函数。

下一节会更具体地展开其中最常见的一类：调用 `extern "C"` 函数，以及处理裸指
针。

---

[← 上一章：方法与 `this`](ch05-03-methods-and-this.md) · [目录](README.md)
