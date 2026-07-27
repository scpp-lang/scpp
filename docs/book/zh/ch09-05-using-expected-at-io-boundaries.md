# 在 I/O 边界上使用 `std::expected<T, E>`

第 9.4 节展示了：当数据已经进入程序之后，`expected` 怎样在多步函数之间继续传播。
真实程序还需要另一类边界：把外部文本第一次变成带类型的值。

在今天的 scpp 里，这个边界通常同样是显式的：先用 `scpp::io::getline()` 之类的辅
助函数读入文本，再去解析它，并把可能的失败转换成程序剩余部分可以使用的错误枚
举。

下面每个可运行示例都请保存为 `expected-io.scpp`，然后先这样构建：

```sh
scpp expected-io.scpp -o expected-io
```

本节每个示例都会给出各自的输入命令，就写在输出块之前。

## 把“读一行并解析”包装成一个 `expected` 辅助函数

程序通常更希望调用方直接请求“从标准输入里读一个受检查的 `int`”，而不是在每个
地方都重复写读行和解析逻辑。

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class input_error {
    eof,
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, input_error> read_count() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        input_error code = input_error::read_failed;
        if (line.error() == scpp::io::error::eof) {
            code = input_error::eof;
        }
        std::unexpected<input_error> err{code};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<input_error> err{input_error::invalid};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<input_error> err{input_error::out_of_range};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<input_error> err{input_error::trailing_text};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }

    std::expected<int, input_error> result{value};
    return std::move(result);
}

void show_one() {
    auto value = read_count();
    if (value.has_value()) {
        std::println("count = {}", value.value());
        return;
    }
    if (value.error() == input_error::eof) {
        std::println("input closed");
        return;
    }
    std::println("invalid input");
}

int main() {
    show_one();
    show_one();
    show_one();
    return 0;
}
```

这样运行：

```sh
printf '12\n7x\n' | ./expected-io
```

输出：

```text
count = 12
invalid input
input closed
```

这样一来，程序剩下的部分就不必再直接关心 `scpp::io::getline()` 或
`std::from_chars` 了。它看到的只是一个统一的“成功或错误”边界。

## 当调用方要的是“零个或多个值”时，把 EOF 当成普通控制流

并不是每个输入边界都应该把文件结束当成错误。有时 EOF 只是一个正常信号，表示已
经没有更多记录可读了。

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class input_error {
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<std::optional<int>, input_error> read_count_or_eof() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        if (line.error() == scpp::io::error::eof) {
            std::optional<int> done{};
            std::expected<std::optional<int>, input_error> result{done};
            return std::move(result);
        }
        std::unexpected<input_error> err{input_error::read_failed};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<input_error> err{input_error::invalid};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<input_error> err{input_error::out_of_range};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<input_error> err{input_error::trailing_text};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }

    std::optional<int> boxed{value};
    std::expected<std::optional<int>, input_error> result{boxed};
    return std::move(result);
}

int main() {
    int total = 0;
    while (true) {
        auto next = read_count_or_eof();
        if (!next.has_value()) {
            std::println("bad input");
            return 1;
        }
        if (!next.value().has_value()) {
            break;
        }
        total = total + next.value().value();
    }
    std::println("{}", total);
    return 0;
}
```

这样运行：

```sh
printf '10\n20\n30\n' | ./expected-io
```

输出：

```text
60
```

这里 `std::optional<int>` 回答的是另一个问题：`expected` 表示“这次读取本身有没
有成功”，而 `optional` 表示“这次成功的读取是不是还产生了下一个值，还是已经正
常到达输入结束”。

## 把输入细节翻译成更简单的应用层错误枚举

外层 API 不一定想直接暴露所有底层输入细节。它可以先在一个辅助函数里完成读取与
解析，再把这些细节映射成更适合应用层的、更小的错误词汇。

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    eof,
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> read_count() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        count_error code = count_error::read_failed;
        if (line.error() == scpp::io::error::eof) {
            code = count_error::eof;
        }
        std::unexpected<count_error> err{code};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
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

enum class config_error {
    missing_line,
    bad_number,
    number_out_of_range,
};

std::expected<int, config_error> read_port() {
    auto count = read_count();
    if (!count.has_value()) {
        config_error code = config_error::bad_number;
        if (count.error() == count_error::eof) {
            code = config_error::missing_line;
        }
        if (count.error() == count_error::out_of_range) {
            code = config_error::number_out_of_range;
        }
        std::unexpected<config_error> err{code};
        std::expected<int, config_error> result{err};
        return std::move(result);
    }

    int port = count.value();
    if (port < 1 || port > 65535) {
        std::unexpected<config_error> err{config_error::number_out_of_range};
        std::expected<int, config_error> result{err};
        return std::move(result);
    }

    std::expected<int, config_error> result{port};
    return std::move(result);
}

int main() {
    auto good = read_port();
    auto missing = read_port();

    if (good.has_value()) {
        std::println("port = {}", good.value());
    }
    if (!missing.has_value() && missing.error() == config_error::missing_line) {
        std::println("missing port");
    }
    return 0;
}
```

这样运行：

```sh
printf '8080\n' | ./expected-io
```

输出：

```text
port = 8080
missing port
```

内层辅助函数知道 I/O 关闭、解析失败和尾随文本这些细节。外层辅助函数只暴露这个
配置 API 真正关心的那些区分。

## 今天在输入边界上使用 `expected` 的实用规则

- 让一个辅助函数统一负责原始边界工作：读取文本并解析它；
- 尽早把底层 I/O 和解析结果转换成带类型的错误枚举；
- EOF 到底该算错误还是正常结束，取决于你正在设计的 API；
- 当这个区别很重要时，`std::expected<std::optional<T>, E>` 可以同时表达“操作有
  没有成功？”和“还有没有下一个值？”；
- 让程序剩余部分只处理带类型的值和 `expected` 结果，而不是到处重复原始输入检
  查。

这让今天 scpp 的可恢复错误故事形成了一个完整闭环：外部文本先在一个显式边界上
立刻变成带类型的结果，然后作为普通的、受检查的控制流继续穿过程序的其余部分。

---

[← 上一章：在多步 API 里传播 `std::expected<T, E>`](ch09-04-propagating-expected-results.md) · [目录](README.md) · [下一章：如何在 fail-fast、`std::optional<T>` 与 `std::expected<T, E>` 之间做选择 →](ch09-06-choosing-between-fail-fast-optional-and-expected.md)
