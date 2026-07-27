# 用 `std::expected<T, E>` 表达可恢复错误

第 9.2 节里，我们用 `std::optional<T>` 表示“某个操作可能有值，也可能没有值”。
当“缺少值”就是全部信息时，这样很好用。

但有时调用方还需要更多信息。解析可能因为一种原因失败，查找可能因为另一种原
因失败，工厂函数也可能用一个明确的错误码拒绝非法输入。这种场景下，今天的 scpp
使用 `std::expected<T, E>`：**成功时持有一个 `T`，失败时持有一个 `E`。**

下面每个可运行示例都请保存为 `expected.scpp`，然后这样构建并运行：

```sh
scpp expected.scpp -o expected
./expected
```

对于应该被拒绝的示例，如果你想让编译器输出逐字匹配，请把文件名保存成诊断块里
显示的那个描述性名字。

对于应该 abort 的示例，程序本身可以成功编译，但在它向 `expected` 取错那一侧结
果时会终止运行。

## 当失败原因本身也重要时，返回 `std::expected<T, E>`

函数可以返回一个真实值，也可以返回一个领域内的错误码。

```cpp
import std;

enum class divide_error { division_by_zero };

std::expected<int, divide_error> divide_checked(int total, int count) {
    if (count == 0) {
        std::unexpected<divide_error> err{divide_error::division_by_zero};
        std::expected<int, divide_error> result{err};
        return std::move(result);
    }
    std::expected<int, divide_error> result{total / count};
    return std::move(result);
}

int main() {
    auto good = divide_checked(42, 7);
    auto bad = divide_checked(42, 0);

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == divide_error::division_by_zero) {
        std::println("bad divisor");
    }
    return 0;
}
```

输出：

```text
6
bad divisor
```

它和 `std::optional<T>` 的差别在于第二条信息通道。调用方得到的不只是“这里没有
`int`”，还会知道到底发生了**哪一种**可恢复错误。

## 库里的工厂函数也可以返回“可用值”或“错误枚举”

这种模式并不只适用于玩具示例。当前库代码里已经在这样做。

```cpp
import std;
import scpp;

int main() {
    auto ready = scpp::rand::uniform_int_distribution<int>::make(4, 9);
    auto broken = scpp::rand::uniform_int_distribution<int>::make(9, 4);

    if (!ready.has_value()) {
        return 1;
    }
    std::println("{} {}", ready.value().min(), ready.value().max());

    if (!broken.has_value() && broken.error() == scpp::rand::error::empty_range) {
        std::println("empty range");
    }
    return 0;
}
```

输出：

```text
4 9
empty range
```

第一次调用返回一个可正常使用的 distribution 对象。第二次则返回明确的
`scpp::rand::error::empty_range` 错误码。这就是 `std::expected<T, E>` 的典型形
状：调用方先分支一次，然后带着正确的状态继续往下走。

## 丢弃 `expected` 结果会被拒绝

可恢复错误结果要求你显式处理。当前的 `std::expected<T, E>` 带有
`[[nodiscard]]`，所以把结果直接扔掉会成为编译期错误。

```cpp
import std;

enum class divide_error { division_by_zero };

std::expected<int, divide_error> divide_checked(int total, int count) {
    if (count == 0) {
        std::unexpected<divide_error> err{divide_error::division_by_zero};
        std::expected<int, divide_error> result{err};
        return std::move(result);
    }
    std::expected<int, divide_error> result{total / count};
    return std::move(result);
}

int main() {
    divide_checked(42, 0);
    return 0;
}
```

编译器输出：

```text
expected-discard-fail.scpp:16:5: error: discarded return value of nodiscard type 'std::expected.int.divide_error': expected results must be checked
```

所以调用方不能悄悄忽略“成功或错误”这个状态。即使程序最后决定把好几种错误合并
成同一种处理方式，也必须先明确检查这个返回的 `expected`。

## `value()` 和 `error()` 都是受检查的访问

只有在代码先判断它当前持有哪种状态时，`expected` 才真正把失败变成可恢复的。若
对象当前持有的是错误态，再去取 value，程序仍然会 abort。

```cpp
import std;

enum class divide_error { division_by_zero };

int main() {
    std::unexpected<divide_error> err{divide_error::division_by_zero};
    std::expected<int, divide_error> result{err};
    return result.value();
}
```

运行时行为：程序会 abort，因为 `result` 当前持有的是错误，而不是一个 `int`。

反过来也一样：如果对象当前持有的是值态，却去调用 `error()`，也会对称地 abort。

## 今天 `std::expected<T, E>` 的实用规则

- 如果你只关心“有没有一个 `T`”，就用 `std::optional<T>`；
- 如果调用方还需要知道失败原因，就用 `std::expected<T, E>`；
- 成功态直接从一个 `T` 构造，错误态则从 `std::unexpected<E>` 构造；
- 在调用 `value()` 或 `error()` 之前，先用 `has_value()` 分支；
- 丢弃 `expected` 结果会在编译期被拒绝；
- 今天的写法仍然是显式分支，而不是隐藏起来的异常流。

有了 `std::optional<T>` 和 `std::expected<T, E>`，今天的 scpp 已经有了两种清晰
的可恢复错误工具：一种表达“也许有值”，另一种表达“要么有值，要么有原因”。以后
的章节可以在这个基础上继续讨论更大的多步 API，以及结果如何逐层传播。

---

[← 上一章：今天可用的可恢复错误写法](ch09-02-recoverable-errors-today.md) · [目录](README.md) · [下一章：在多步 API 里传播 `std::expected<T, E>` →](ch09-04-propagating-expected-results.md)
