# 如何在 fail-fast、`std::optional<T>` 与 `std::expected<T, E>` 之间做选择

到这里，Chapter 9 里已经摆出了三种不同的错误处理工具：

- 由编译器插入、在运行时 fail-fast 的受检查操作；
- 用来表达普通“可能没有值”的 `std::optional<T>`；
- 用来表达“可能失败且要说明原因”的 `std::expected<T, E>`。

剩下的问题已经不再是“它们怎么工作”，而是“API 什么时候该选哪一种”。

下面每个可运行示例都请保存为 `api-shapes.scpp`，然后这样构建并运行：

```sh
scpp api-shapes.scpp -o api-shapes
./api-shapes
```

对于应该 abort 的示例，程序本身可以成功编译，但一旦执行走到那个非法状态就会终
止。

## 当坏状态意味着调用方违约或不该发生的状态时，用 fail-fast 的受检查操作

有些函数只有在前置条件本来就成立时才有意义。对这种情况，今天的 scpp 往往直接依
赖普通的受检查操作：如果有人错误调用，就让程序 abort，而不是把它伪装成一个可恢
复返回值。

```cpp
import std;

int items_per_group(int total_items, int groups) {
    return total_items / groups;
}

int main() {
    std::println("{}", items_per_group(12, 3));
    return 0;
}
```

输出：

```text
4
```

这里的 `groups == 0` 并没有被当成一种“正常业务结果”。它表示调用方违约了。如果执
行真的走到 `items_per_group(12, 0)`，程序会在那个受检查的除法上 abort，而不是假
装调用方还能继续正常运行。

## 当“没有值”本身是正常情况，而且不需要额外解释时，用 `std::optional<T>`

如果调用方唯一关心的问题只是“有没有这个值”，那么 `std::optional<T>` 往往就是最
简单的形状。

```cpp
import std;

std::optional<int> find_channel(int id) {
    if (id == 7) {
        std::optional<int> found{42};
        return found;
    }
    std::optional<int> missing{};
    return missing;
}

int main() {
    auto first = find_channel(7);
    auto second = find_channel(1);

    if (first.has_value()) {
        std::println("{}", first.value());
    }
    if (!second.has_value()) {
        std::println("missing");
    }
    return 0;
}
```

输出：

```text
42
missing
```

调用方并不需要一个错误码来说明 channel `1` 为什么不存在。它只需要知道：这里没有
对应的映射。

## 当调用方需要根据不同失败原因做不同分支时，用 `std::expected<T, E>`

如果调用方需要针对不同失败原因写出不同处理逻辑，那就返回 `expected`，并把那个
原因也放进类型里。

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class port_error {
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, port_error> parse_port(const std::string& text) {
    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<port_error> err{port_error::invalid};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<port_error> err{port_error::out_of_range};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<port_error> err{port_error::trailing_text};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }

    if (value < 1 || value > 65535) {
        std::unexpected<port_error> err{port_error::out_of_range};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }

    std::expected<int, port_error> result{value};
    return std::move(result);
}

int main() {
    auto good = parse_port("8080");
    auto bad = parse_port("80x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == port_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

输出：

```text
8080
trailing text
```

这就是为什么这里要选 `expected` 而不是 `optional`：调用方不仅能知道“失败了”，还
能知道“失败的是哪一种原因”。

## 今天 scpp 的 API 设计实用规则

到这里，这一章的决策树已经相当小了：

- 如果走到坏状态意味着 bug、前置条件被破坏，或者本来就不该发生的执行路径，那
  就依赖普通受检查操作并 fail-fast；
- 如果“没有值”是正常情况，而且不需要额外说明，就返回 `std::optional<T>`；
- 如果“失败”是正常情况，而且调用方需要一个带类型的失败原因，就返回
  `std::expected<T, E>`；
- 把这些选择直接写进函数签名里，而不是用异常把它们藏起来。

这就是今天 scpp 里错误处理的主要形状：bug 走受检查的 fail-fast 执行，可恢复情况
走显式返回值，而类型签名会直接告诉调用方：自己面对的到底是哪一类情况。

---

[← 上一章：在 I/O 边界上使用 `std::expected<T, E>`](ch09-05-using-expected-at-io-boundaries.md) · [目录](README.md) · [下一章：泛型函数与 class →](ch10-01-generic-functions-and-classes.md)
