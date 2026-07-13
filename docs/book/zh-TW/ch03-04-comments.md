# 註解

註解是寫給人看的。它可以用來解釋意圖、標出一個容易忽略的細節，或是在程式碼旁邊
留下一句短短的提醒，而不會改變編譯器真正執行的程式。

所以註解這個主題雖然不大，卻很重要：好的註解，常常決定了一段程式碼只是「能跑」，
還是「過一段時間回來看也能很快看懂」。

下面每個短小示例都可以存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 行註解

最常見的形式，是以 `//` 開頭的行註解。

```cpp
import std;

int main() {
    // This comment is for people, not for the compiler.
    int answer = 40 + 2;

    std::println("answer = {}", answer);
    return 0;
}
```

輸出：

```text
answer = 42
```

在那一行裡，`//` 後面的內容都會被編譯器忽略。

## 區塊註解

如果一行短說明不夠，就可以用區塊註解。

```cpp
import std;

int main() {
    /* A block comment can cover more than one line
       when one short note is not enough. */
    int total = 20 + 22;

    std::println("total = {}", total);
    return 0;
}
```

輸出：

```text
total = 42
```

區塊註解很適合拿來放稍微長一點、而且確實應該緊貼在那段程式碼旁邊的說明。

## 寫在語句旁邊的短註解

有時候，最清楚的說明就是直接跟在那條語句後面的一個小提醒。

```cpp
import std;

int main() {
    int score = 7; // starting score for this round
    score = score + 5;

    std::println("score = {}", score);
    return 0;
}
```

輸出：

```text
score = 12
```

這種寫法最適合非常簡短的說明。如果解釋開始變長，就最好把註解移到語句上方。

## 一條實用規則

好的註解更應該解釋「為什麼」，而不只是重複「做了什麼」。如果程式碼本身已經很清楚
地表達它正在做什麼，再用文字重講一遍，往往只會增加雜訊。更值得寫註解的場景是：

- 程式碼本身看不出來的意圖；
- 讀者應該知道的前提假設；
- 在更長的函式或模組裡，幫助快速定位的說明。

下一節會重新回到程式行為和控制流程：怎樣用 `if` 在不同路徑之間做選擇，以及怎樣用
`while` 重複一段工作。

---

[← 上一章：函式](ch03-03-functions.md) · [目錄](README.md) · [下一章：控制流程 →](ch03-05-control-flow.md)
