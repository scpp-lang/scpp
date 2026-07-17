# `[[scpp::unsafe]]` 會做什麼、不會做什麼

scpp 的設計目標，是讓絕大多數程式碼都留在語言受檢查的普通區域裡。不過有時
候，程式確實需要做一些編譯器無法自行證明安全的事，例如解參考裸指標，或者呼叫
一個未受檢查的外部函式。

這就是 `[[scpp::unsafe]]` 的用途。

同樣重要的是，`[[scpp::unsafe]]` 被刻意設計得很窄。它**不會**關閉所有權檢查、
借用檢查或生命週期檢查。它只會打開少數幾個需要程式設計師在局部自己承擔理由的
安全邊界。

下面每個可執行範例都可以存成 `unsafe.scpp`，然後這樣建置並執行：

```sh
scpp unsafe.scpp -o unsafe
./unsafe
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸
出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 使用 unsafe block

最常見的形式，是一個 unsafe block。block 外面的程式碼仍然是普通的安全 scpp 程
式碼。

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

輸出：

```text
42
```

像 `&value` 這樣形成裸指標，在安全程式碼裡本來就是允許的。真正的 unsafe 邊界
出現在程式決定「信任這個指標並解參考它」的那一刻。

## 同樣的操作在 `[[scpp::unsafe]]` 外會被拒絕

如果你試圖在普通安全程式碼裡解參考這個裸指標，編譯器就會攔下你。

```cpp
int read_value(int* pointer) {
    return *pointer;
}

int main() {
    int value{42};
    return read_value(&value);
}
```

編譯器輸出：

```text
raw_pointer_unsafe_fail.scpp:2:12: error: cannot dereference raw pointer 'pointer': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)
 2 |     return *pointer;
   |            ^
```

所以，`[[scpp::unsafe]]` 不是風格提示。它是真正決定某些操作是否構成良構程式的
門檻。

## 你也可以把整個函式標成 unsafe

如果某個輔助函式的全部意義，就是執行一種 unsafe 操作，那麼你也可以直接把屬性
寫在函式上。

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

輸出：

```text
9
```

這表示 `read_first` 的整個函式主體都處在 unsafe context 裡。但這**不**表示呼叫
端就自動安全了。

## 呼叫 unsafe 函式本身也需要 unsafe context

被標記為 unsafe 的函式，自帶一個未受檢查的前提條件，因此呼叫點也必須明確承認
這一點。

```cpp
[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    return read_first(&value);
}
```

編譯器輸出：

```text
call_unsafe_function_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

這正是這套設計的核心思想：unsafe 假設應該留在程式真正依賴它們的那個位置上，保
持可見。

## `[[scpp::unsafe]]` 不會關閉借用檢查

unsafe 程式碼仍然會繼續受到所有權與別名規則的檢查。

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

編譯器輸出：

```text
unsafe_still_checks_borrows_fail.scpp:5:9: error: cannot mutably borrow 'value': it is already borrowed
 5 |         int& second = value;
   |         ^
```

所以，`[[scpp::unsafe]]` **並不**等於「把檢查器關掉」。它的意思只是「在這裡允許
語言中那些被明確設門的操作之一」。

## `[[scpp::unsafe]]` 主要是拿來做什麼的

從高層看，`[[scpp::unsafe]]` 是 scpp 允許你跨越少數幾個明確邊界的位置，例如：

- 解參考裸指標，或者做指標算術；
- 呼叫沒有函式主體的 `extern "C"` 函式；
- 存取 union 成員；
- 使用原始的 `new` 或 `delete`；
- 呼叫那些自身宣告就帶有 `[[scpp::unsafe]]` 的函式。

下一節會更具體地展開其中最常見的一類：呼叫 `extern "C"` 函式，以及處理裸指
標。

---

[← 上一章：方法與 `this`](ch05-03-methods-and-this.md) · [目錄](README.md)
