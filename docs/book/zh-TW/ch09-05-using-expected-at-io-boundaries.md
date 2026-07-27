# 在 I/O 邊界上使用 `std::expected<T, E>`

第 9.4 節展示了：當資料已經進入程式之後，`expected` 怎樣在多步函式之間繼續傳
遞。真實程式還需要另一類邊界：把外部文字第一次變成帶型別的值。

在今天的 scpp 裡，這個邊界通常同樣是顯式的：先用 `scpp::io::getline()` 之類的輔
助函式讀入文字，再去解析它，並把可能的失敗轉換成程式剩餘部分可以使用的錯誤列
舉。

下面每個可執行範例都請存成 `expected-io.scpp`，然後先這樣建置：

```sh
scpp expected-io.scpp -o expected-io
```

本節每個範例都會給出各自的輸入命令，就寫在輸出區塊之前。

## 把「讀一行並解析」包成一個 `expected` 輔助函式

程式通常更希望呼叫端直接請求「從標準輸入裡讀一個受檢查的 `int`」，而不是在每個
地方都重複寫讀行和解析邏輯。

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

這樣執行：

```sh
printf '12\n7x\n' | ./expected-io
```

輸出：

```text
count = 12
invalid input
input closed
```

這樣一來，程式剩下的部分就不必再直接關心 `scpp::io::getline()` 或
`std::from_chars` 了。它看到的只是一個統一的「成功或錯誤」邊界。

## 當呼叫端要的是「零個或多個值」時，把 EOF 當成普通控制流

並不是每個輸入邊界都應該把檔案結束當成錯誤。有時 EOF 只是一個正常訊號，表示已
經沒有更多記錄可讀了。

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

這樣執行：

```sh
printf '10\n20\n30\n' | ./expected-io
```

輸出：

```text
60
```

這裡 `std::optional<int>` 回答的是另一個問題：`expected` 表示「這次讀取本身有沒
有成功」，而 `optional` 表示「這次成功的讀取是不是還產生了下一個值，還是已經正
常到達輸入結束」。

## 把輸入細節翻譯成更簡單的應用層錯誤列舉

外層 API 不一定想直接暴露所有底層輸入細節。它可以先在一個輔助函式裡完成讀取與
解析，再把這些細節映射成更適合應用層的、更小的錯誤詞彙。

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

這樣執行：

```sh
printf '8080\n' | ./expected-io
```

輸出：

```text
port = 8080
missing port
```

內層輔助函式知道 I/O 關閉、解析失敗和尾隨文字這些細節。外層輔助函式只暴露這個
設定 API 真正關心的那些區分。

## 今天在輸入邊界上使用 `expected` 的實用規則

- 讓一個輔助函式統一負責原始邊界工作：讀取文字並解析它；
- 盡早把底層 I/O 和解析結果轉換成帶型別的錯誤列舉；
- EOF 到底該算錯誤還是正常結束，取決於你正在設計的 API；
- 當這個區別很重要時，`std::expected<std::optional<T>, E>` 可以同時表達「操作有
  沒有成功？」和「還有沒有下一個值？」；
- 讓程式剩餘部分只處理帶型別的值和 `expected` 結果，而不是到處重複原始輸入檢
  查。

這讓今天 scpp 的可恢復錯誤故事形成了一個完整閉環：外部文字先在一個顯式邊界上
立刻變成帶型別的結果，然後作為普通的、受檢查的控制流繼續穿過程式的其餘部分。

---

[← 上一章：在多步 API 裡傳遞 `std::expected<T, E>`](ch09-04-propagating-expected-results.md) · [目錄](README.md)
