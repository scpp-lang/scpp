# 如何把「信任」局部化到真實程式裡

前兩節解釋了 `[[scpp::unsafe]]` 到底替哪些事情開門，以及裸指標、`extern "C"` 呼
叫是如何跨過這道邊界的。

這一節要回答下一個更實際的問題：**在真實程式裡，unsafe 程式碼應該放在哪裡？**

總規則其實很簡單：

- 讓 unsafe 區域盡可能小；
- 如果一個函式能完全為自己的 unsafe 操作負責，就優先寫成普通安全函式，在內部
  放一個很小的 unsafe block；
- 只有當函式的正確性依賴於呼叫端必須滿足、而函式主體自己無法證明的前提條件
  時，才使用函式層級的 `[[scpp::unsafe]]`。

下面每個可執行範例都可以存成 `trust.scpp`，然後這樣建置並執行：

```sh
scpp trust.scpp -o trust
./trust
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸
出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 優先在普通安全函式裡放一個很小的 unsafe block

如果一個函式本身就能完全控制並證明那次 unsafe 操作是合理的，那麼就讓這個函式
保持普通，只把真正關鍵的那一步放進 unsafe。

```cpp
import std;

int first_of_pair(int left, int right) {
    int values[2]{};
    values[0] = left;
    values[1] = right;
    int* pointer = &values[0];
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    std::println("{}", first_of_pair(11, 22));
    return 0;
}
```

輸出：

```text
11
```

這裡，呼叫端根本不需要知道裸指標的存在。這個函式自己建立了區域陣列、形成了指
標，也只在一個很小的位置解參考了它，所以它可以直接為那一步負責。

## 當呼叫端必須擔保時，使用函式層級的 `[[scpp::unsafe]]`

有時候，函式主體自己無法獨立讓操作變得可靠。如果函式接收的是外部給來的裸指
標，那麼它的正確性就取決於一個只有呼叫端才能保證的前提條件。

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

這裡的 unsafe，不再只是內部實作細節，而是這個函式契約的一部分。

## 這個契約會傳播到呼叫點

因為真正需要擔保輸入的人是呼叫端，所以從安全程式碼裡直接呼叫這種函式，會被編
譯器拒絕。

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
call_unsafe_wrapper_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

所以，函式層級的 `[[scpp::unsafe]]` 應該被看成一個很明確的 API 設計決定。它會
讓呼叫端也分擔安全論證的責任。

## 即使在較大的封裝裡，也要把 unsafe 邊界壓窄

真實程式裡，往往需要連續呼叫幾個外部函式，但規則仍然一樣：讓每個 unsafe 區域
盡量貼近真正需要它的那一次呼叫，讓其餘邏輯繼續保持普通安全程式碼。

```cpp
import std;

extern "C" {
    int socket(int domain, int type, int protocol);
    int getsockopt(int fd, int level, int optname, void* optval, int* optlen);
    int close(int fd);
}

int query_socket_type() {
    int fd = 0;
    [[scpp::unsafe]] {
        fd = socket(2, 2, 0);
    }

    int value = 0;
    int len = 4;
    [[scpp::unsafe]] {
        getsockopt(fd, 1, 3, &value, &len);
        close(fd);
    }
    return value;
}

int main() {
    std::println("{}", query_socket_type());
    return 0;
}
```

輸出：

```text
2
```

`query_socket_type` 的大部分程式碼仍然是普通程式碼：區域變數、傳回值和控制流程
都沒有變。真正被圍起來的，只有那些外部呼叫本身。

## 即使整個 unsafe 區域很大，檢查器也仍然開著

把整個程式碼區塊標成 unsafe，**並不**代表關閉所有權檢查。即使有時你確實需要一
個較寬的 unsafe 區域，scpp 仍然會繼續檢查 move 和 borrow。

```cpp
import std;

int f() {
    [[scpp::unsafe]] {
        std::unique_ptr<int> first = std::make_unique<int>(1);
        std::unique_ptr<int> second = std::move(first);
        std::unique_ptr<int> third = std::move(first);
        return *third;
    }
}

int main() {
    return f();
}
```

編譯器輸出：

```text
unsafe_whole_body_still_checks_moves_fail.scpp:7:38: error: use of moved-out variable 'first'
 7 |         std::unique_ptr<int> third = std::move(first);
   |                                      ^
```

這才是「把信任局部化」的真正含義：unsafe 的部分應該只包含那些編譯器確實無法自
行證明的地方，其餘部分仍然應該盡量留在普通受檢查的世界裡。

下一章會離開這些底層邊界，轉向專案結構：套件、模組與資訊清單檔。

---

[← 上一章：呼叫 `extern "C"` 與使用裸指標](ch06-02-calling-extern-c-and-using-raw-pointers.md) · [目錄](README.md)
