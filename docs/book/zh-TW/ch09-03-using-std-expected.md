# 用 `std::expected<T, E>` 表達可恢復錯誤

第 9.2 節裡，我們用 `std::optional<T>` 表示「某個操作可能有值，也可能沒有值」。
當「缺少值」就是全部資訊時，這樣很好用。

但有時呼叫端還需要更多資訊。解析可能因為一種原因失敗，查找可能因為另一種原因
失敗，工廠函式也可能用一個明確的錯誤碼拒絕非法輸入。這種情況下，今天的 scpp
使用 `std::expected<T, E>`：**成功時持有一個 `T`，失敗時持有一個 `E`。**

下面每個可執行範例都請存成 `expected.scpp`，然後這樣建置並執行：

```sh
scpp expected.scpp -o expected
./expected
```

對於應該被拒絕的範例，如果你想讓編譯器輸出逐字對上，請把檔名存成診斷區塊裡顯
示的描述性名稱。

對於應該 abort 的範例，程式本身可以成功編譯，但在它向 `expected` 取錯那一側結
果時會終止執行。

## 當失敗原因本身也很重要時，回傳 `std::expected<T, E>`

函式可以回傳一個真實值，也可以回傳一個領域內的錯誤碼。

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

輸出：

```text
6
bad divisor
```

它和 `std::optional<T>` 的差別在於第二條資訊通道。呼叫端得到的不只是「這裡沒有
`int`」，還會知道到底發生了**哪一種**可恢復錯誤。

## 函式庫裡的工廠函式也可以回傳「可用值」或「錯誤列舉」

這種模式並不只適用於玩具範例。現在的函式庫程式碼裡已經在這樣做。

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

輸出：

```text
4 9
empty range
```

第一次呼叫回傳一個可以正常使用的 distribution 物件。第二次則回傳明確的
`scpp::rand::error::empty_range` 錯誤碼。這就是 `std::expected<T, E>` 的典型形
狀：呼叫端先分支一次，然後帶著正確的狀態繼續往下走。

## 丟棄 `expected` 結果會被拒絕

可恢復錯誤結果要求你顯式處理。當前的 `std::expected<T, E>` 帶有
`[[nodiscard]]`，所以把結果直接丟掉會變成編譯期錯誤。

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

編譯器輸出：

```text
expected-discard-fail.scpp:16:5: error: discarded return value of nodiscard type 'std::expected.int.divide_error': expected results must be checked
```

所以呼叫端不能悄悄忽略「成功或錯誤」這個狀態。即使程式最後決定把好幾種錯誤合
併成同一種處理方式，也必須先明確檢查這個回傳的 `expected`。

## `value()` 和 `error()` 都是受檢查的存取

只有在程式先判斷它目前持有哪種狀態時，`expected` 才真的把失敗變成可恢復的。若
物件目前持有的是錯誤態，再去取 value，程式仍然會 abort。

```cpp
import std;

enum class divide_error { division_by_zero };

int main() {
    std::unexpected<divide_error> err{divide_error::division_by_zero};
    std::expected<int, divide_error> result{err};
    return result.value();
}
```

執行時行為：程式會 abort，因為 `result` 目前持有的是錯誤，而不是一個 `int`。

反過來也一樣：如果物件目前持有的是值態，卻去呼叫 `error()`，也會對稱地 abort。

## 今天 `std::expected<T, E>` 的實用規則

- 如果你只關心「有沒有一個 `T`」，就用 `std::optional<T>`；
- 如果呼叫端還需要知道失敗原因，就用 `std::expected<T, E>`；
- 成功態直接從一個 `T` 建構，錯誤態則從 `std::unexpected<E>` 建構；
- 在呼叫 `value()` 或 `error()` 之前，先用 `has_value()` 分支；
- 丟棄 `expected` 結果會在編譯期被拒絕；
- 今天的寫法仍然是顯式分支，而不是藏起來的例外流程。

有了 `std::optional<T>` 和 `std::expected<T, E>`，今天的 scpp 已經有了兩種清晰
的可恢復錯誤工具：一種表達「也許有值」，另一種表達「要嘛有值，要嘛有原因」。之
後的章節可以在這個基礎上繼續討論更大的多步 API，以及結果如何逐層傳遞。

---

[← 上一章：目前可用的可恢復錯誤寫法](ch09-02-recoverable-errors-today.md) · [目錄](README.md) · [下一章：在多步 API 裡傳遞 `std::expected<T, E>` →](ch09-04-propagating-expected-results.md)
