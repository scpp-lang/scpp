# 純量資料型別

上一節重點講的是「變數該怎麼宣告」。這一節把視線轉向這些變數裡面到底裝著什麼值。

對剛開始寫 scpp 程式的人來說，先掌握四種純量型別，就已經夠做很多事了：

- `int`：整數；
- `double`：帶小數部分的數；
- `bool`：真 / 假判斷；
- `char`：單一字元。

和前一節一樣，下面這些短小示例都可以存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 整數與小數

`int` 通常是計數、索引以及其他整數計算的起點；而當分數 / 小數部分真的重要時，
`double` 會是更常見的選擇。

```cpp
import std;

int main() {
    int left = 10 - 3;
    double price = 1.25 + 0.5;

    std::println("left = {}, price = {}", left, price);
    return 0;
}
```

輸出：

```text
left = 7, price = 1.75
```

注意這裡的兩個計算各自待在自己的型別裡：`int` 運算式完全是整數運算，`double` 運
算式完全是浮點運算。

如果之後你需要的不是 `int` / `double` 這種比較順手的寫法，而是精確位寬，那麼 scpp
也提供了 `int32_t`、`uint64_t`、`float64_t` 之類的名稱。

## `bool` 應該來自真正的條件

在 scpp 裡，條件本身就應該已經是 `bool`。比較運算式會直接給出這樣的 `bool` 值。

```cpp
import std;

int main() {
    int lives = 3;
    bool keep_playing = lives > 0;

    if (keep_playing) {
        std::println("keep playing");
        return 0;
    }

    std::println("game over");
    return 0;
}
```

輸出：

```text
keep playing
```

這和普通 C++ 有一個細小但很重要的差別：scpp 不會要求你把任意整數當成「truthy /
falsey」來理解。你應該明確寫出像 `lives > 0` 這樣的比較，再把結果存進 `bool` 裡。

## `char` 表示單一字元

當一個位元組大小的單一字元值就足夠時，就用 `char`。

```cpp
import std;

int main() {
    char grade = 'A';

    std::println("grade = {}", grade);
    return 0;
}
```

輸出：

```text
grade = A
```

這裡單引號很重要。`'A'` 是一個 `char`；而 `"A"` 表示的是文字，不是單一字元值。

## 再記住一條規則

scpp 會把這些純量型別清楚區分開來。`bool`、`char`、`int`、`double` 不會靜默地互相
轉換。所以前面的例子才會讓每個運算式都維持在同一種純量型別裡，並在需要 `bool`
時寫出真正的比較。

一開始這會比普通 C++ 更嚴格一點，但它也讓每一次計算到底是什麼型別，讀起來更一目
了然。

下一節會繼續沿著這些基礎往前走，把計算和動作打包進具名函式裡。

---

[← 上一章：變數與明確初始化](ch03-01-variables-and-explicit-initialization.md) · [目錄](README.md) · [下一章：函式 →](ch03-03-functions.md)
