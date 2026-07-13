# 控制流程

變數負責保存值，函式負責把一段工作包裝成可重用的部件，而控制流程決定接下來要執行
哪一段工作，以及它要執行幾次。

在目前的 scpp 裡，學習者現在真正能用上的主要控制流程工具包括 `if`、`while`、經典
`for`，以及基於範圍的 `for`。這一節會用一些你今天就能寫、也能跑得起來的小程式，
依序介紹它們。

下面每個短小示例都可以存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## `if` 只會在條件為真時執行程式碼

當一段程式碼只應該在某個條件成立時執行，就用 `if`。

```cpp
import std;

int main() {
    int temperature = 33;

    if (temperature > 30) {
        std::println("It is warm outside.");
    }

    return 0;
}
```

輸出：

```text
It is warm outside.
```

`if (...)` 裡的條件本身就應該已經是一個 `bool`。這裡的 `temperature > 30` 是一個比
較運算式，因此它產生的正是 `if` 所需要的那種值。

## `else if` 與 `else` 用來在不同路徑之間做選擇

當程式有不只一條可能路徑時，可以用 `else if` 把條件串起來，再用 `else` 處理最後的
兜底情況。

```cpp
import std;

int main() {
    int score = 85;

    if (score < 60) {
        std::println("try again");
    } else if (score < 90) {
        std::println("you passed");
    } else {
        std::println("excellent");
    }

    return 0;
}
```

輸出：

```text
you passed
```

這些分支會從上到下依序檢查。一旦有某個條件為真，對應的分支就會執行，後面的分支也
就不會再看了。

## `while` 會在條件維持為真時重複執行工作

當同一段程式碼應該一直重複，直到某個條件不再成立時，就用 `while`。

```cpp
import std;

int main() {
    int count = 3;

    while (count > 0) {
        std::println("{}!", count);
        count = count - 1;
    }

    std::println("Go!");
    return 0;
}
```

輸出：

```text
3!
2!
1!
Go!
```

一個 `while` 迴圈既需要條件，也需要會改變的狀態。如果 `count` 從來不變，這個迴圈
就永遠不會結束。

## 經典 `for` 會把迴圈設定集中寫在一起

經典 `for` 會把三個部分放進同一個迴圈標頭裡：

- 只執行一次的初始步驟；
- 每一輪開始前檢查的條件；
- 每一輪完成後執行的更新步驟。

```cpp
import std;

int main() {
    int total = 0;

    for (int i = 1; i <= 5; i = i + 1) {
        total = total + i;
    }

    std::println("total = {}", total);
    return 0;
}
```

輸出：

```text
total = 15
```

這和你用 `while` 寫計數迴圈時表達的是同一個想法，但 `for` 會把迴圈變數、停止條件
和每輪更新放在一起，更容易一眼看清楚。

## 基於範圍的 `for` 會依序走過每個元素

如果你想把陣列裡的每個元素都走訪一次，基於範圍的 `for` 往往是最清楚的寫法。

```cpp
import std;

int main() {
    int scores[3]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    int total = 0;

    for (int score : scores) {
        total = total + score;
    }

    std::println("total = {}", total);
    return 0;
}
```

輸出：

```text
total = 60
```

這裡的迴圈變數 `score` 會依序用陣列裡的每個元素來初始化。因為它是按值宣告的，所以
就算你修改 `score` 本身，也不會改動底層陣列。

## 基於範圍的 `for` 也可以搭配 `std::span`

基於範圍的 `for` 不只適用於固定大小陣列。它也可以搭配 `std::span` 使用；`std::span`
是 scpp 裡用來借用一段元素序列的視圖型別。

```cpp
import std;

int main() {
    int values[3]{};
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    std::span<int> view = values;

    for (auto& value : view) {
        value = value * 2;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

輸出：

```text
2
4
6
```

因為迴圈變數寫成了 `auto&`，所以每一次迭代裡的 `value` 都是在參照 `span` 背後那個
真正的元素。你更新 `value`，原本的陣列也會一起被更新。

## 一條實用規則

在決定該用哪一種控制流程工具時，可以先記住這幾條：

- 想在不同路徑之間做選擇，就用 `if`；
- 想重複一段工作，就用 `while`；
- 如果一個迴圈天生就是「初始化 + 條件 + 更新」，就用經典 `for`；
- 如果你想依序走訪陣列或 `std::span` 的每個元素，就用基於範圍的 `for`；
- 把條件寫成明確的比較，讓它本身直接產生 `bool`；
- 如果迴圈最終應該停下來，就要更新那個被條件讀取的狀態。

這已經足夠幫助你理解前面猜數字章節裡見過的所有示例，也足夠寫出很多會分支、會重複、
會計數，也會逐一處理序列元素的小型命令列程式。

---

[← 上一章：註解](ch03-04-comments.md) · [目錄](README.md) · [下一章：什麼是所有權？→](ch04-01-what-is-ownership.md)
