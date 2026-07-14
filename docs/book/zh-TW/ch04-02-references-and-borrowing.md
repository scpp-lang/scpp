# 參照與借用

所有權解釋的是：誰負責清理資源。參照解釋的是：程式怎樣才能在**不接走這份責任**的前提下，暫時使用某個值。

在 scpp 裡，這裡用的就是原生 C++ 參照語法：

- `const T&` 表示共享、唯讀借用；
- `T&` 表示可變借用。

這裡沒有 Rust 風格的 `&mut` 關鍵字。相反地，scpp 會在普通 C++ 參照語法之上，再由 `movecheck` 執行受 Rust 啟發的借用檢查。

下面每個可執行範例都可以存成 `references.scpp`，然後這樣建置並執行：

```sh
scpp references.scpp -o references
./references
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 用 `const T&` 借用

如果一個函式只需要「看一眼」某個值，就可以接收 `const T&`，而不是接走所有權。

```cpp
import std;

int calculate_length(const std::string& text) {
    return text.length();
}

int main() {
    std::string title{"scpp"};
    int length{calculate_length(title)};
    std::println("{} has {} bytes", title.c_str(), length);
    return 0;
}
```

輸出：

```text
scpp has 4 bytes
```

呼叫結束後，`title` 仍然可以繼續使用，因為所有權根本沒有移進 `calculate_length`。函式只是把它借用了過去。

這就是「借用」這個詞的意思：被呼叫者只拿到暫時的存取權，呼叫者仍然是擁有者。

## 經由 `const T&` 的直接寫入會被拒絕

共享借用是唯讀的。

```cpp
import std;

void change(const int& value) {
    value = 2;
    return;
}

int main() {
    int x{1};
    change(x);
    return 0;
}
```

編譯器會拒絕它：

```text
assign_through_const_ref_fail.scpp:4:5: error: cannot assign through 'value': it is a read-only (const) reference
 4 |     value = 2;
   |     ^
```

## 用 `T&` 做可變借用

如果一個函式需要修改呼叫者的值、但又不想成為它的擁有者，就用 `T&`。

```cpp
import std;

void add_suffix(std::string& text) {
    text.append(" book");
    return;
}

int main() {
    std::string title{"scpp"};
    add_suffix(title);
    std::println("{}", title.c_str());
    return 0;
}
```

輸出：

```text
scpp book
```

`add_suffix` 並沒有接走 `title` 的所有權。它只是以可變方式借用了 `title`，原地修改之後就回傳了。整個過程中，擁有者始終還是呼叫者。

## 任意多個共享借用，或者一個可變借用

scpp 對安全別名施加的核心限制，正是你會希望看到的這個形狀：

- 任意多個 `const T&` 借用可以同時存在；
- 同一時刻最多只能有一個 `T&` 借用；
- 同一個值的共享借用和可變借用不能重疊。

多個共享借用沒有問題：

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    const int& second = value;
    std::println("{}", first + second);
    return 0;
}
```

輸出：

```text
10
```

但如果一個共享借用還活著，再去建立同一個值的可變借用，就會被拒絕：

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    int& second = value;
    return first + second;
}
```

編譯器輸出：

```text
shared_and_mut_fail.scpp:6:5: error: cannot mutably borrow 'value': it is already borrowed
 6 |     int& second = value;
   |     ^
```

同樣地，第二個 `T&` 借用同一個物件也會被編譯器拒絕。

這條限制真正保證的是：程式可以擁有一批共享讀者，或者擁有一個可變寫者，但不能兩者同時存在。

## 從既有參照再借一次：reborrow

如果一個新參照**是從另一個參照形成的**，那它就是一個 reborrow。

這裡可以看到 scpp 現在的規則，比「整個程式區塊都算借用還活著」這種樸素說法更精細。如果一個可變 lender 派生出 child borrow，scpp 會按「最後一次可能使用」來判斷這個 child borrow 還算不算 *live*。

只要這個 child borrow 還 live：

- 仍然允許透過 lender 去讀；
- 不允許透過 lender 去寫；
- 不允許再從同一個 lender 形成新的 reborrow。

透過 lender 去讀是可以的：

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    std::println("{}", lender + child);
    return 0;
}
```

輸出：

```text
10
```

但如果 child 還 live，就不能再透過 lender 去寫：

```cpp
import std;

int main() {
    int value{1};
    int& first = value;
    int& second = first;
    first = 2;
    std::println("{} {}", first, second);
    return 0;
}
```

編譯器輸出：

```text
reborrow_write_lender_fail.scpp:7:5: error: cannot write through 'first' while a nested reborrow derived from it is still live
 7 |     first = 2;
   |     ^
```

類似地，如果此時再寫 `int& third = first;`，也會被拒絕：

```text
reborrow_further_reborrow_fail.scpp:7:5: error: cannot form another reborrow from 'first' while a nested reborrow derived from it is still live
 7 |     int& third = first;
   |     ^
```

一旦 child borrow 的最後一次使用已經過去，lender 就會重新變得可寫；即使整個程式區塊還沒結束，也是如此：

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    int before = child;
    lender = 7;
    int& second = lender;
    second = second + 1;
    std::println("{} {}", before, second);
    return 0;
}
```

輸出：

```text
5 8
```

這裡 `lender = 7;` 能通過，是因為 `child` 的最後一次使用已經在 `int before = child;` 那一行發生完了。scpp 檢查的是「最後一次使用之後是否還活著」，而不只是「詞法作用域有沒有結束」。

## 參照必須始終有效

參照絕不能比它所指向的物件活得更久。

今天的 scpp v0.1 會用一種偏保守的方式，在函式回傳處執行這條規則：如果函式回傳參照，編譯器就必須能把這個回傳參照的生命週期，推斷為來自某個輸入參照形參。因此，函式不能從一個區域物件裡「現造一個參照」再把它回傳出去。

```cpp
import std;

const std::string& bad() {
    std::string local{"oops"};
    return local;
}

int main() {
    const std::string& ref = bad();
    std::println("{}", ref.c_str());
    return 0;
}
```

編譯器輸出：

```text
return_local_ref_fail.scpp:3:1: error: function 'bad' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- refactor to take a single reference parameter, or return by value/std::unique_ptr instead
 3 | const std::string& bad() {
   | ^
```

最直接的修正方式，就是把擁有的值本身回傳出去：

```cpp
import std;

std::string make_title() {
    std::string local{"scpp"};
    return local;
}

int main() {
    std::string title{make_title()};
    std::println("{}", title.c_str());
    return 0;
}
```

輸出：

```text
scpp
```

## 參照規則小結

到這裡，工作規則可以整理成：

- `const T&` 會借用一個值而不接走所有權，而且直接經由它寫入會被拒絕；
- `T&` 同樣是在借用、不接走所有權，但它允許修改；
- 一個值要嘛同時擁有任意多個共享借用，要嘛同時只有一個可變借用；
- live 的 reborrow 會阻止經由 lender 寫入，也會阻止繼續從 lender 形成新的 reborrow，但不會阻止經由 lender 讀取；
- reborrow 的 live 範圍由最後一次使用決定，而不只由程式區塊結尾決定；
- 參照必須始終有效，而今天的 scpp 只有在能從輸入參照推斷生命週期時，才接受「回傳參照」。

下一節會把同一套借用模型，套用到「面向一段元素範圍的非擁有視圖」上。

---

[← 上一章：什麼是所有權？](ch04-01-what-is-ownership.md) · [目錄](README.md)
