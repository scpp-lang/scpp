# 在多步 API 裡傳遞 `std::expected<T, E>`

第 9.3 節介紹了 `std::expected<T, E>` 這個基礎的「值或錯誤」結果型別。下一步的
問題就是：更大的函式應該怎樣使用它？

今天的 scpp 不會在 `expected` 之上額外加上例外或專門的傳遞運算子。多步程式碼仍
然是顯式的：檢查每一個結果，需要時提早回傳錯誤，只有在成功態時才繼續往下做。

下面每個可執行範例都請存成 `expected-propagation.scpp`，然後這樣建置並執行：

```sh
scpp expected-propagation.scpp -o expected-propagation
./expected-propagation
```

本節裡的所有範例都應該能成功編譯並執行。

## 先把底層狀態式 API 包成一個回傳 `expected` 的輔助函式

底層函式庫函式可能會用別的形狀回報失敗。一個常見做法是先在邊界處轉換一次，然
後讓程式剩下的部分直接統一使用 `std::expected<T, E>`。

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

輸出：

```text
128
trailing text
```

`std::from_chars` 本身是透過 `ec` 和 `ptr` 欄位回報結果的。這個輔助函式把它轉成
一個統一的 `expected<int, count_error>` 邊界，之後呼叫端就能一直停留在同一種結果
風格裡。

## 當兩層使用同一種錯誤列舉時，直接提早回傳向上傳遞

如果外層函式和它呼叫的輔助函式使用同一種錯誤型別，那麼顯式傳遞通常就是「先檢
查，再把錯誤態原樣回傳上去」。

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

輸出：

```text
42
trailing text
```

這就是今天可用的手動傳遞模式。這裡沒有隱藏跳轉，也沒有例外路徑；函式把失敗是
在哪裡回傳給呼叫端寫得一清二楚。

## 在更寬的 API 邊界上，把底層錯誤翻譯成更高層的錯誤

有時一個更大的 API 希望暴露的錯誤詞彙比內部輔助函式更簡單。這時外層程式碼會檢
查每一個底層結果，再把它映射成自己承諾對外提供的錯誤碼。

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

輸出：

```text
6
bad count
bad range
```

外層函式把詳細的解析規則藏在內部。呼叫端只需要知道：到底是請求文字有問題，還
是請求出來的範圍本身不合法。

## 今天 `expected` 傳遞的實用規則

- 能在邊界處把底層「狀態式」API 轉成一個 `std::expected<T, E>`，就盡量先轉；
- 如果兩層共用同一個錯誤列舉，就先檢查 `has_value()`，再顯式把錯誤回傳上去；
- 如果更寬的 API 想暴露更少或不同的錯誤，就在外層邊界做翻譯；
- 成功路徑程式碼放在 `has_value()` 檢查之後，只有在那條路徑上才去存取 `value()`；
- 今天還沒有專門的傳遞運算子：結果流就是普通控制流本身。

這種顯式風格很適合現在的 scpp。控制流保持可見，可恢復情況保持型別化，而每個函
式也都能自己決定要向外暴露多少底層錯誤細節。

---

[← 上一章：用 `std::expected<T, E>` 表達可恢復錯誤](ch09-03-using-std-expected.md) · [目錄](README.md) · [下一章：在 I/O 邊界上使用 `std::expected<T, E>` →](ch09-05-using-expected-at-io-boundaries.md)
