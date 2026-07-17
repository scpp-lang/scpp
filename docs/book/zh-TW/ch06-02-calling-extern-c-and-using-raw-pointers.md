# 呼叫 `extern "C"` 與使用裸指標

上一節介紹了 `[[scpp::unsafe]]`：它是 scpp 用來打開少數幾個明確未受檢查操作的窄
門。

其中最常見的兩類，就是：

- 呼叫一個沒有函式主體的 `extern "C"` 函式；
- 解參考一個裸指標。

這一節就專門聚焦在這兩類操作上，以及即使在 unsafe 程式碼裡也依然成立的那一點
點型別資訊。

下面每個可執行範例都可以存成 `raw-pointers.scpp`，然後這樣建置並執行：

```sh
scpp raw-pointers.scpp -o raw-pointers
./raw-pointers
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸
出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 形成裸指標本身是安全的

用 `&value` 取位址，本身屬於普通安全程式碼。真正需要 `[[scpp::unsafe]]` 的，是
*信任* 這個指標並解參考它。

```cpp
import std;

int main() {
    int value{1};
    int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 9;
    }
    std::println("{}", value);
    return 0;
}
```

輸出：

```text
9
```

這種分工是刻意設計的。安全程式碼可以先為底層 API 準備好裸指標，但真正的解參考
仍然要放在一個明確的 unsafe 邊界之後。

## 不帶 unsafe 的裸指標解參考會被拒絕

如果你試圖在安全程式碼裡直接解參考裸指標，編譯器就會攔下你。

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

## 呼叫一個沒有函式主體的 `extern "C"` 函式

沒有函式主體的 `extern "C"` 宣告，也是另一種未受檢查邊界。scpp 看不到它的實
作，因此呼叫它同樣需要 unsafe context。

```cpp
import std;

extern "C" int abs(int x);

int main() {
    [[scpp::unsafe]] {
        std::println("{}", abs(-7));
    }
    return 0;
}
```

輸出：

```text
7
```

這裡的設計模式和裸指標是同一個：宣告這個邊界本身沒有問題，但真正去信任它時，
就必須寫出 `[[scpp::unsafe]]`。

## 在安全程式碼裡呼叫這個 `extern "C"` 函式會被拒絕

如果這次呼叫發生在 unsafe context 之外，編譯器就會拒絕它。

```cpp
extern "C" int abs(int x);

int main() {
    return abs(-7);
}
```

編譯器輸出：

```text
calling_extern_c_requires_unsafe_fail.scpp:4:12: error: cannot call 'extern "C"' function 'abs' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real implementation to check it (spec ch01 §1.3/ch02)
 4 |     return abs(-7);
   |            ^
```

## 可變指標可以擴寬成 `const` 指標

普通的指標型別規則依然成立。一個可變的 `T*`，可以傳給需要 `const T*` 的地方。

```cpp
import std;

int read(const int* pointer) {
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    int value{7};
    int* pointer = &value;
    std::println("{}", read(pointer));
    return 0;
}
```

輸出：

```text
7
```

所以，`[[scpp::unsafe]]` 並不會抹掉型別系統。它只會給某些特定操作開門。

## 即使在 unsafe block 裡，透過 `const` 指標寫入仍然是型別錯誤

哪怕放進 unsafe block，`const int*` 也仍然是唯讀的。

```cpp
int main() {
    int value{5};
    const int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 10;
    }
    return value;
}
```

編譯器輸出：

```text
write_through_const_pointer_fail.scpp:5:9: error: cannot assign to this place: it is reached through a read-only (const) reference
 5 |         *pointer = 10;
   |         ^
```

## 對唯讀位置取位址會得到 `const T*`

同一條規則在「形成指標」時也會出現。如果來源位置只能透過 `const` 到達，那麼得到
的指標型別就必須是 `const T*`，而不能是 `T*`。

```cpp
int read(const int& value) {
    int* pointer = &value;
    return 0;
}

int main() {
    int number{1};
    return read(number);
}
```

編譯器輸出：

```text
address_of_const_ref_fail.scpp:2:20: error: cannot assign '&' of a read-only-reachable place to 'pointer' (a mutable 'T*'): would need 'const T*', which 'pointer' isn't declared as
 2 |     int* pointer = &value;
   |                    ^
```

所以，scpp 裡的裸指標雖然是底層工具，但它們並不是「無型別」的。一個指標是可變還
是 `const`，在任何地方都依然重要。

下一節會繼續停留在 unsafe 這一章，但會把重點從單個呼叫、單次解參考的機制，轉
到「在真實程式裡如何把信任局部化」這個更大的問題上。

---

[← 上一章：`[[scpp::unsafe]]` 會做什麼、不會做什麼](ch06-01-what-scpp-unsafe-does-and-does-not-do.md) · [目錄](README.md)
