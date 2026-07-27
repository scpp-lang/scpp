# 如何在 fail-fast、`std::optional<T>` 與 `std::expected<T, E>` 之間做選擇

到這裡，Chapter 9 裡已經擺出了三種不同的錯誤處理工具：

- 由編譯器插入、在執行時 fail-fast 的受檢查操作；
- 用來表達普通「可能沒有值」的 `std::optional<T>`；
- 用來表達「可能失敗且要說明原因」的 `std::expected<T, E>`。

剩下的問題已經不再是「它們怎麼工作」，而是「API 什麼時候該選哪一種」。

下面每個可執行範例都請存成 `api-shapes.scpp`，然後這樣建置並執行：

```sh
scpp api-shapes.scpp -o api-shapes
./api-shapes
```

對於應該 abort 的範例，程式本身可以成功編譯，但一旦執行走到那個非法狀態就會終
止。

## 當壞狀態意味著呼叫端違約或不該發生的狀態時，用 fail-fast 的受檢查操作

有些函式只有在前置條件本來就成立時才有意義。對這種情況，今天的 scpp 往往直接依
賴普通的受檢查操作：如果有人錯誤呼叫，就讓程式 abort，而不是把它偽裝成一個可恢
復回傳值。

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

輸出：

```text
4
```

這裡的 `groups == 0` 並沒有被當成一種「正常業務結果」。它表示呼叫端違約了。如果
執行真的走到 `items_per_group(12, 0)`，程式會在那個受檢查的除法上 abort，而不是
假裝呼叫端還能繼續正常執行。

## 當「沒有值」本身是正常情況，而且不需要額外解釋時，用 `std::optional<T>`

如果呼叫端唯一關心的問題只是「有沒有這個值」，那麼 `std::optional<T>` 往往就是最
簡單的形狀。

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

輸出：

```text
42
missing
```

呼叫端並不需要一個錯誤碼來說明 channel `1` 為什麼不存在。它只需要知道：這裡沒
有對應的映射。

## 當呼叫端需要根據不同失敗原因做不同分支時，用 `std::expected<T, E>`

如果呼叫端需要針對不同失敗原因寫出不同處理邏輯，那就回傳 `expected`，並把那個
原因也放進型別裡。

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

輸出：

```text
8080
trailing text
```

這就是為什麼這裡要選 `expected` 而不是 `optional`：呼叫端不只知道「失敗了」，還
能知道「失敗的是哪一種原因」。

## 今天 scpp 的 API 設計實用規則

到這裡，這一章的決策樹已經相當小了：

- 如果走到壞狀態意味著 bug、前置條件被破壞，或者本來就不該發生的執行路徑，那
  就依賴普通受檢查操作並 fail-fast；
- 如果「沒有值」是正常情況，而且不需要額外說明，就回傳 `std::optional<T>`；
- 如果「失敗」是正常情況，而且呼叫端需要一個帶型別的失敗原因，就回傳
  `std::expected<T, E>`；
- 把這些選擇直接寫進函式簽名裡，而不是用例外把它們藏起來。

這就是今天 scpp 裡錯誤處理的主要形狀：bug 走受檢查的 fail-fast 執行，可恢復情況
走顯式回傳值，而型別簽名會直接告訴呼叫端：自己面對的到底是哪一類情況。

---

[← 上一章：在 I/O 邊界上使用 `std::expected<T, E>`](ch09-05-using-expected-at-io-boundaries.md) · [目錄](README.md)
