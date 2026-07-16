# `std::span` 與其他非擁有視圖

所有權解釋的是誰負責清理資源。參照解釋的是：程式怎樣才能在不接走這份責任的前提下，暫時使用一個值。`std::span<T>` 則把同一套「借用」思路套用到一整段連續元素上。

在今天的 scpp 裡，`std::span` 是最主要的標準非擁有視圖型別。你可以把一個 span 理解成一個很小的視圖值：它把指向首元素的指標和長度配在一起。span 本身並不擁有這些元素；真正的擁有者仍然是那個陣列。

下面每個可執行範例都可以存成 `span.scpp`，然後這樣建置並執行：

```sh
scpp span.scpp -o span
./span
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 從固定大小陣列建構 span

今天，正常的建構路徑就是從固定大小陣列開始。

```cpp
import std;

int main() {
    int numbers[4]{};
    numbers[0] = 7;
    numbers[1] = 8;
    numbers[2] = 9;
    numbers[3] = 10;
    std::span<int> view = numbers;
    int length = view.size;
    std::println("{}", length);
    std::println("{}", view[2]);
    return 0;
}
```

輸出：

```text
4
9
```

`view` 借用的是這個陣列。建構這個 span 時，四個元素並沒有被複製，所有權也沒有從 `numbers` 身上移走。

## 把 span 傳給函式做唯讀存取，而不複製元素

如果一個函式只需要讀取一段序列，就可以接收 `std::span<const T>`。

```cpp
import std;

int sum(std::span<const int> values) {
    int total = 0;
    for (int value : values) {
        total = total + value;
    }
    return total;
}

int main() {
    int numbers[4]{};
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    std::println("sum = {}", sum(numbers));
    return 0;
}
```

輸出：

```text
sum = 100
```

`sum(numbers)` 這個呼叫會在呼叫點建構出 span 視圖。把 span 按值傳進去時，被複製的只是這個很小的視圖物件，而不是底層陣列裡的元素。

## 可變 span 可以更新呼叫者的陣列

如果一個函式需要原地修改既有元素，就接收 `std::span<T>`。

```cpp
import std;

void double_all(std::span<int> values) {
    for (auto& value : values) {
        value = value * 2;
    }
    return;
}

int main() {
    int numbers[3]{};
    numbers[0] = 3;
    numbers[1] = 4;
    numbers[2] = 5;
    double_all(numbers);
    for (int value : numbers) {
        std::println("{}", value);
    }
    return 0;
}
```

輸出：

```text
6
8
10
```

`double_all` 依然不會擁有這個陣列。它拿到的是一個可變的非擁有視圖，經由這個視圖完成寫入，而整個過程中擁有者始終還是呼叫者。

## `std::span<const T>` 是唯讀的

一旦元素型別寫成 `const`，得到的就是共享、唯讀視圖。

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<const int> view = numbers;
    view[0] = 99;
    return 0;
}
```

編譯器輸出：

```text
span_const_write_fail.scpp:6:10: error: cannot assign to this place: it is reached through a read-only (const) reference
 6 |     view[0] = 99;
   |          ^
```

這條規則和上一節裡的 `const T&` 完全一樣：共享借用允許讀取，但不允許寫入。

## span 借用遵循與參照相同的 live 規則

第 4.2 節裡的借用模型，在這裡仍然適用。一個共享 span 最後一次使用結束後，同一個陣列就可以開始一個可變 span 借用。

```cpp
import std;

int main() {
    int numbers[3]{};
    numbers[0] = 5;
    numbers[1] = 6;
    numbers[2] = 7;
    std::span<const int> reader = numbers;
    int first = reader[0];
    std::span<int> writer = numbers;
    writer[1] = 9;
    std::println("{} {}", first, numbers[1]);
    return 0;
}
```

輸出：

```text
5 9
```

這裡 `writer` 這個借用之所以被接受，是因為 `reader` 的最後一次使用已經在 `int first = reader[0];` 那一行結束了。

但如果共享 span 借用和可變 span 借用發生重疊，就會被拒絕：

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<int> writer = numbers;
    std::span<const int> reader = numbers;
    return writer[0] + reader[0];
}
```

編譯器輸出：

```text
span_borrow_conflict_fail.scpp:6:5: error: cannot borrow 'numbers': it is already mutably borrowed
 6 |     std::span<const int> reader = numbers;
   |     ^
```

所以，span 並不是繞過所有權檢查的逃生艙。它是視圖，但它依然是借用。

## 目前版本的限制

如果你想圍繞 span 設計 API，今天有兩個限制特別重要。

第一，建構目前仍然只接受固定大小陣列：

```cpp
import std;

int main() {
    int value{1};
    std::span<int> view = value;
    return 0;
}
```

編譯器輸出：

```text
span_non_array_fail.scpp:5:27: error: std::span<T> can currently only be constructed from a fixed-size array in this version
 5 |     std::span<int> view = value;
   |                           ^
```

第二，span 目前還不能在初始化之後重新繫結：

```cpp
import std;

int main() {
    int first[2]{};
    int second[2]{};
    std::span<int> view = first;
    view = second;
    return 0;
}
```

編譯器輸出：

```text
span_reassign_fail.scpp:7:5: error: std::span 'view' cannot be reassigned after initialization in this version
 7 |     view = second;
   |     ^
```

所以今天的 `std::span` 更像一個「永久繫結」的借用，而不是一個可以自由重新指定的視圖值。

## `std::span` 規則小結

到這裡，工作規則可以整理成：

- `std::span<T>` 是一個面向連續元素的非擁有視圖；
- 建構或傳遞 span 時，不會複製底層元素；
- `std::span<const T>` 是唯讀的，而 `std::span<T>` 允許修改；
- 第 4.2 節裡的借用與 live 規則，同樣適用於 span；
- 今天的 span 由固定大小陣列建構，而且建構之後不能重新繫結。

後面的陣列章節還會更詳細地回到緩衝區與視圖這個主題。

---

[← 上一章：參照與借用](ch04-02-references-and-borrowing.md) · [目錄](README.md)
