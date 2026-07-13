# 變數與明確初始化

在猜數字小遊戲那一章裡，我們已經用區域變數記住了秘密數字和玩家最近一次的猜測。
現在可以把節奏放慢一點，單獨看看這些宣告本身到底代表什麼。

這一節裡的短小示例都可以存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 區域變數預設就是可變的

一個普通的區域變數會替某個值取一個名字，而那個值之後仍然可以在同一個作用域裡改
變。

```cpp
import std;

int main() {
    int counter = 0;
    counter = counter + 1;
    counter = counter + 1;

    std::println("counter = {}", counter);
    return 0;
}
```

輸出：

```text
counter = 2
```

這裡真正重要的不只是 `counter` 的值變了，還在於：它在整個生命週期裡始終維持同一
個型別。它一旦宣告成 `int`，之後就一直是 `int`。

## `const` 會讓區域變數變成唯讀

如果某個區域變數只應該在宣告時賦值一次、之後保持不變，就寫上 `const`。

```cpp
import std;

int main() {
    const int target = 21;
    int doubled = target + target;

    std::println("doubled = {}", doubled);
    return 0;
}
```

輸出：

```text
doubled = 42
```

`target` 會在宣告時完成初始化，之後就不能再次賦值了。只要一個名字代表的是你不希
望被意外改掉的值，這種寫法就很好用。

## 每個區域變數都必須帶初始化器

現在的 scpp 不允許寫出 `int score;` 這種「裸宣告」的區域變數。每個區域變數在宣告
時都必須把自己的起始值寫清楚。

```cpp
import std;

int main() {
    int score{};
    int level{3};
    int bonus = 7;
    bool finished{};

    std::println("score = {}, level = {}, bonus = {}, finished = {}", score, level, bonus, finished);
    return 0;
}
```

輸出：

```text
score = 0, level = 3, bonus = 7, finished = false
```

這個小例子順手展示了最常見的三種寫法：

- `int score{};` 用空大括號，表示「取這個型別的零值 / 預設值」；
- `int level{3};` 用帶實參的大括號，明確給出起始資料；
- `int bonus = 7;` 用 `=` 接一個運算式來初始化。

真正重要的規則是：初始化器不是可選項。哪怕你想要的只是零值或 `false`，scpp 也要
求你在宣告處明確寫出來。

## 一個簡單的日常習慣

在日常程式裡，先抓住這四條就夠了：

- 需要改變的值，用普通區域變數；
- 初始化後就不該再變的值，用 `const`；
- 每個區域變數都在宣告時立刻初始化，不要先宣告、後補值；
- 想要零值 / 預設值時用 `{}`，已經知道起始資料時用 `= value` 或 `{value}`。

下一節會繼續保持這種「小程式、慢拆解」的節奏，但重點會更直接地放到這些變數所承
載的資料型別本身。

---

[← 上一章：做一個猜數字小遊戲](ch02-00-guessing-game.md) · [目錄](README.md) · [下一章：純量資料型別 →](ch03-02-scalar-data-types.md)
