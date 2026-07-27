# 在多步 API 里传播 `std::expected<T, E>`

第 9.3 节介绍了 `std::expected<T, E>` 这个基础的“值或错误”结果类型。下一步的
问题就是：更大的函数应该怎样使用它？

今天的 scpp 并不会在 `expected` 之上额外加上异常或专门的传播运算符。多步代码仍
然是显式的：检查每一个结果，需要时提早返回错误，只有在成功态时才继续往下做。

下面每个可运行示例都请保存为 `expected-propagation.scpp`，然后这样构建并运行：

```sh
scpp expected-propagation.scpp -o expected-propagation
./expected-propagation
```

本节里的所有示例都应该能够成功编译并运行。

## 先把底层状态式 API 包装成一个返回 `expected` 的辅助函数

底层库函数可能会用别的形状报告失败。一个常见做法是先在边界处转换一次，然后让
程序剩下的部分直接统一使用 `std::expected<T, E>`。

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

int main() {
    auto good = parse_count("128");
    auto bad = parse_count("12x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == count_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

输出：

```text
128
trailing text
```

`std::from_chars` 本身是通过 `ec` 和 `ptr` 字段报告结果的。这个辅助函数把它转换
成一个统一的 `expected<int, count_error>` 边界，之后调用方就能一直停留在同一种
结果风格里。

## 当两层使用同一种错误枚举时，直接提早返回向上传播

如果外层函数和它调用的辅助函数使用同一种错误类型，那么显式传播通常就是“先检
查，再把错误态原样返回上去”。

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

std::expected<int, count_error> double_count(const std::string& text) {
    auto parsed = parse_count(text);
    if (!parsed.has_value()) {
        std::unexpected<count_error> err{parsed.error()};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{parsed.value() * 2};
    return std::move(result);
}

int main() {
    auto good = double_count("21");
    auto bad = double_count("7x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == count_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

输出：

```text
42
trailing text
```

这就是今天可用的手动传播模式。这里没有隐藏跳转，也没有异常路径；函数把失败是
在什么地方返回给调用方写得一清二楚。

## 在更宽的 API 边界上，把底层错误翻译成更高层的错误

有时一个更大的 API 希望暴露的错误词汇比内部辅助函数更简单。这时外层代码会检查
每一个底层结果，再把它映射成自己承诺对外提供的错误码。

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

enum class request_error {
    bad_count,
    bad_range,
};

std::expected<int, request_error> load_width(const std::string& text) {
    auto parsed = parse_count(text);
    if (!parsed.has_value()) {
        std::unexpected<request_error> err{request_error::bad_count};
        std::expected<int, request_error> result{err};
        return std::move(result);
    }

    auto distribution = scpp::rand::uniform_int_distribution<int>::make(1, parsed.value());
    if (!distribution.has_value()) {
        std::unexpected<request_error> err{request_error::bad_range};
        std::expected<int, request_error> result{err};
        return std::move(result);
    }

    int width = distribution.value().max() - distribution.value().min() + 1;
    std::expected<int, request_error> result{width};
    return std::move(result);
}

int main() {
    auto good = load_width("6");
    auto bad_count = load_width("6x");
    auto bad_range = load_width("0");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad_count.has_value() && bad_count.error() == request_error::bad_count) {
        std::println("bad count");
    }
    if (!bad_range.has_value() && bad_range.error() == request_error::bad_range) {
        std::println("bad range");
    }
    return 0;
}
```

输出：

```text
6
bad count
bad range
```

外层函数把详细的解析规则藏在内部。调用方只需要知道：到底是请求文本有问题，还
是请求出来的范围本身不合法。

## 今天 `expected` 传播的实用规则

- 能在边界处把底层“状态式”API 转成一个 `std::expected<T, E>`，就尽量先转；
- 如果两层共用同一个错误枚举，就先检查 `has_value()`，再显式把错误返回上去；
- 如果更宽的 API 想暴露更少或不同的错误，就在外层边界做翻译；
- 成功路径代码放在 `has_value()` 检查之后，只有在那条路径上才去访问 `value()`；
- 今天还没有专门的传播运算符：结果流就是普通控制流本身。

这种显式风格很适合当前的 scpp。控制流保持可见，可恢复情况保持类型化，而每个函
数也都能自己决定要向外暴露多少底层错误细节。

---

[← 上一章：用 `std::expected<T, E>` 表达可恢复错误](ch09-03-using-std-expected.md) · [目录](README.md)
