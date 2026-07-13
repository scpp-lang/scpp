# 函式

變數會替值命名；函式則會替可以重複做的工作命名。

這正好是資料型別之後最自然的一步：當你已經知道程式裡有哪些值之後，就可以開始把
計算打包成可以重複呼叫的片段。

下面每個短小示例都可以存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 定義並呼叫一個函式

一個函式定義有四個很熟悉的部分：

- 回傳型別；
- 函式名稱；
- 放在括號裡的參數列表；
- 放在大括號裡的函式本體。

```cpp
import std;

int double_value(int value) {
    return value * 2;
}

int main() {
    int doubled = double_value(21);

    std::println("doubled = {}", doubled);
    return 0;
}
```

輸出：

```text
doubled = 42
```

`double_value(21)` 這次呼叫會把 `21` 交給它的參數，然後從函式裡拿回一個 `int` 結
果。

## 參數本身也是區域名稱

在函式本體裡，每個參數都可以看成一個帶有初始值的區域變數。

```cpp
import std;

int add_one(int value) {
    value = value + 1;
    return value;
}

int main() {
    int score = 10;
    int next = add_one(score);

    std::println("score = {}, next = {}", score, next);
    return 0;
}
```

輸出：

```text
score = 10, next = 11
```

`add_one` 裡面改動 `value`，並不會反過來改寫 `main` 裡的 `score`。函式處理的是它
自己的參數變數，然後把結果回傳出來。

## 函式也可以回傳 `bool`

函式不一定非得回傳數字。它也可以回傳一個 `bool`，讓程式的其他部分直接拿來判斷。

```cpp
import std;

bool can_level_up(int score, int bonus) {
    return score + bonus >= 100;
}

int main() {
    bool ready = can_level_up(80, 25);

    std::println("ready = {}", ready);
    return 0;
}
```

輸出：

```text
ready = true
```

當一個函式要回答的是「是 / 否問題」，而不是去計算一個新數字或一段文字時，這種寫
法就很合適。

## 再記住一條重要的型別規則

函式的參數型別和回傳型別都是明確寫出來的，而 scpp 也會把不同純量型別清楚區分開
來。這代表：函式呼叫應該傳入它真正要求的型別，而不是依賴隱藏的隱式轉換。

實際寫程式時，這反而會讓函式呼叫更容易閱讀：看一眼函式簽名，就知道那裡應該放什
麼型別的值。

下一節會繼續維持這種「小而實用」的風格，但主題會換成註解：怎樣向人類讀者解釋程
式碼，同時又不改變編譯器真正看到的程式。

---

[← 上一章：純量資料型別](ch03-02-scalar-data-types.md) · [目錄](README.md) · [下一章：註解 →](ch03-04-comments.md)
