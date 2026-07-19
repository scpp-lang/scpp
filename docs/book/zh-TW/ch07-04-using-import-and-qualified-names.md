# 使用 `import` 與限定名稱

自從引入模組以來,每一個範例都手動配對著同樣兩件事:一句為模組命名的
`import`,以及在每一個呼叫處完整寫出來的、以 `::` 限定的路徑,用來存取其中的
內容。這一節單獨研究 `import` 本身——它到底接受哪些形式,它到底會不會、以及
如何,把名字帶入作用域,還有它和一個路徑自己的限定名稱究竟是怎麼配合的。

下面的每一個範例都放在同一個套件裡。

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "mathlib-app"
version = "0.1.0"

[[bin]]
name = "app"
sources = ["src/*.scpp"]
```

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## `import` 命名的是整個模組——從來不是其中單獨一個項目

到目前為止,每一句 `import` 的形狀都完全一樣:關鍵字、一個點分名字、一個分
號。既然路徑已經用 `::` 來存取模組內部某一個具體的項目,很自然會讓人好奇
`import` 是不是也接受同樣的東西——比如說,只從 `mathlib` 裡匯入
`sum_of_squares`,而不是匯入整個模組。

`src/mathlib.scpp`:

```cpp
export module mathlib;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib::sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

編譯器輸出:

```text
src/main.scpp:2:15: error: expected ';' but found '::'
 2 | import mathlib::sum_of_squares;
   |               ^
```

`::` 完全不會出現在一句 `import` 裡——它只會出現在之後,出現在呼叫處的路徑
裡。換成一個點,模仿模組自己多段名字的寫法,倒是能通過解析:

```cpp
import std;
import mathlib.sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

編譯器輸出:

```text
src/main.scpp: error: cannot find module 'mathlib.sum_of_squares' (use --import mathlib.sum_of_squares=path/to/file or -I <dir>)
```

這一次是因為別的原因才失敗的。[在模組樹中參照項目的路徑]
(ch07-03-paths-for-referring-to-items-in-module-tree.md) 說明過,一個點會把
模組自己的名字連接成若干段,就像 `mathlib.trig` 那樣。這裡適用的是同一條規
則:`mathlib.sum_of_squares` 會被理解成一個有兩段的模組名字,`mathlib` 和
`sum_of_squares`——而不是「模組 `mathlib` 裡面那個叫 `sum_of_squares` 的項
目」——而且根本不存在這樣一個模組。`import` 裡的點永遠是另一段模組名字,從來
不是項目選擇符。`import` 沒有任何「局部匯入」的形式:每一句 `import` 要麼完
整地命名一個模組,要麼什麼都命名不了。

## 匯入一個模組,並不會把它的名字不加限定地帶入作用域

既然 `import mathlib;` 是依賴 `mathlib` 的唯一方式,它到底把什麼帶入了作用
域?從第一章開始的每一個範例其實早就回答了這個問題,只是從來沒有被明確指出
來:`import std;` 從來沒有讓後面哪一行直接裸呼叫 `println`——一直都是
`std::println`。任何其他模組也是同樣的道理。

```cpp
import std;
import mathlib;

int main() {
    return sum_of_squares(3, 4);
}
```

編譯器輸出:

```text
src/main.scpp:5:12: error: call to unknown function 'sum_of_squares'
 5 |     return sum_of_squares(3, 4);
   |            ^
```

`mathlib` 確實被匯入了,`sum_of_squares` 也確實從中被匯出了,但裸名字
`sum_of_squares` 在這裡和一個從未被宣告過的名字一樣,完全無法識別。只有限定
形式才能存取到它:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

輸出:

```text
25
```

拿掉 `println` 前面的 `std::`,失敗的原因和方式完全一樣——`std` 和
`mathlib` 一樣,也只是一個被 `import` 帶入作用域的普通模組:

```cpp
import std;

int main() {
    println("{}", 42);
    return 0;
}
```

編譯器輸出:

```text
src/main.scpp:4:5: error: call to unknown function 'println'
 4 |     println("{}", 42);
   |     ^
```

`import` 只會讓一個模組匯出的內容可以透過它們各自完整的路徑存取到。它從來
不會縮短這條路徑,也從來不會把模組的任何名字單獨帶入作用域——標準函式庫也
不例外。

## 同樣的規則適用於每一個被匯出的項目,不只是函式

到目前為止,以這種方式存取到的名字都是函式,但這條規則並不是針對函式的。它
對一個 `struct` 同樣適用。

`src/mathlib.scpp`,在 `sum_of_squares` 旁邊加上一個 `Point`:

```cpp
export module mathlib;

namespace mathlib {
    export struct Point {
        int x;
        int y;
    };

    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    mathlib::Point p{};
    p.x = 3;
    p.y = 4;
    std::println("{}", mathlib::sum_of_squares(p.x, p.y));
    return 0;
}
```

輸出:

```text
25
```

`Point` 是在 `mathlib::Point` 這個它自己完整的限定名稱下建構出來的,和
`sum_of_squares` 在 `mathlib::sum_of_squares` 下被呼叫是同一回事。裸寫
`Point` 在這裡和裸寫 `sum_of_squares` 在上一節裡一樣,都不在作用域內——
`import` 把它們兩個帶入作用域的方式完全一樣,也就是說:只能透過各自完整的
路徑,而與項目本身是什麼種類無關。

## 一個檔案裡的每一句 `import` 都必須出現在其他內容之前

到目前為止,不管是這一節還是前兩節,每一句 `import` 都出現在自己所在檔案的
最前面,在任何其他宣告之前。這並不是一種風格選擇。

```cpp
import std;

int triple(int x) {
    return x * 3;
}

import std;

int main() {
    std::println("{}", triple(4));
    return 0;
}
```

編譯器輸出:

```text
src/main.scpp:7:1: error: expected a type name
 7 | import std;
   | ^
```

第二句 `import std;` 被拒絕了——並不是因為重複匯入 `std` 本身有什麼問題,
而是因為它所在的位置。一旦解析越過了檔案最前面那一整段連續的 `import` 和
`export import`,`import` 就不再被辨識為任何內容的開頭。一個檔案需要的所有
`import`,都必須集中寫在其他宣告之前。

## 普通 `import` 和 `export import` 決定的始終只是誰能存取一個名字

[用模組控制作用域與可見性](ch07-02-control-scope-and-privacy-with-modules.md)
已經講過兩者的區別:普通的 `import name;` 只對寫下它的檔案私有,而
`export import name;` 會把 `name` 自身的匯出內容,以它們各自原本的名字,傳
遞式地重新匯出給轉而匯入當前模組的任何檔案。這個區別原樣成立,和那裡描述的
完全一樣——這裡新加進來要檢驗的,是一個自身名字有不只一段的模組。

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/stats.scpp`,以普通、私有的方式匯入它:

```cpp
export module stats;

import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`,只匯入 `stats`:

```cpp
import std;
import stats;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

編譯器輸出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::sin_deg'
 5 |     return mathlib::trig::sin_deg(30);
   |            ^
```

和單段模組名字的情形完全一樣,`stats.scpp` 自己那句普通的
`import mathlib.trig;` 不會把 `mathlib::trig::sin_deg` 轉發給任何轉而匯入
`stats` 的檔案。把這一行改成 `export import mathlib.trig;`,結果就不一樣了:

`src/stats.scpp`:

```cpp
export module stats;

export import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`,依然只匯入 `stats`:

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    std::println("{}", stats::double_sin_deg(30));
    return 0;
}
```

輸出:

```text
30
60
```

`mathlib::trig::sin_deg` 傳到 `main.scpp` 時,用的仍然是它原本的兩段
路徑——重新匯出一個多段模組,並不會改變它的路徑需要多少段,或者每一段叫什
麼。普通 `import` 和 `export import` 決定的始終只是哪些檔案能沿著一條路徑
存取到內容;兩者都不會改變路徑本身。

## `import` 沒有別名機制

C++ 可以用 `namespace alias = long::qualified::name;` 把一個很長的限定名稱
綁定到一個更短的名字上。scpp 的 `import` 沒有類似的東西。

```cpp
import std;
import mathlib as m;

int main() {
    return m::sum_of_squares(3, 4);
}
```

編譯器輸出:

```text
src/main.scpp:2:16: error: expected ';' but found 'as'
 2 | import mathlib as m;
   |                ^
```

`import` 只有一種形式:關鍵字、一個點分模組名字、一個分號——前面可以選擇性
地加上 `export`。沒有任何東西能在匯入的同時為模組改名,匯入之後也沒有任何
東西能縮短這條路徑。這一節以及前兩節裡的每一個呼叫處,寫出來的都是同一條完
整路徑——模組自己的名字和命名空間已經決定好的那一條,每一次都原樣寫出。

## 到目前為止關於 `import` 與限定名稱的規則

- 一句 `import` 總是完整地命名一整個模組——沒有辦法只匯入其中一個項目,
  `::` 也從不出現在 `import` 這一行本身裡,只會出現在之後的路徑裡;
- 匯入一個模組,無論對哪種項目,都不會把它的任何名字不加限定地帶入作用
  域——存取它們始終需要各自完整的路徑;
- 一個檔案裡的每一句 `import` 都必須出現在其他任何宣告之前;
- 普通 `import` 只讓一個名字在寫下它的檔案內部可以存取;`export import` 依
  然會轉發同一個名字,路徑不變,不管它有多少段;
- scpp 對匯入或者限定名稱都沒有別名機制——每一個呼叫處寫出來的,都是模組自
  己早已選定的那一條路徑。

到目前為止,一個模組始終對應著恰好一個檔案。下一節要看看,一旦一個模組自己
的原始碼需要分散到不只一個檔案裡,哪些東西會變,哪些不會。

---

[← 上一章：在模組樹中參照項目的路徑](ch07-03-paths-for-referring-to-items-in-module-tree.md) · [目錄](README.md)
