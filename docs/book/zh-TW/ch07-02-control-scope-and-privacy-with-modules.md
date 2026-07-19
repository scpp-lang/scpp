# 用模組控制作用域與可見性

上一節始終停留在套件這一層：清單裡的 `[[bin]]` 和 `[[lib]]` 表，以及 `scpp
build` 如何讓同一個套件裡的執行檔共享另一個目標建置出來的模組。這一節要往下深
入到語言本身：一旦某個執行檔可以 `import` 一個模組，它到底能拿到些什麼？

簡短的答案是：拿到的東西比整個檔案要少得多。一個模組自己的原始碼裡，儘是些永
遠不會離開這個檔案的普通宣告。只有同時滿足下面兩個條件，一個宣告才會對匯入方
可見：

- 它被標記為 `export`；
- 它被宣告在一個和模組自身名字相符的命名空間裡。

只要有一個條件沒滿足，這個宣告就仍然是私有的——在模組自己的檔案內部可以正常使
用，在其他任何地方都不可見。

下面的每一個範例都放在同一個套件裡。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "mathlib-app"
version = "0.1.0"

[[bin]]
name = "app"
sources = ["src/*.scpp"]
```

這裡只用一個帶 glob `sources` 模式的 `[[bin]]` 目標就夠了：正如[套件與專案清
單](ch07-01-packages-and-project-manifests.md)一節講過的，一個目標的
`sources` 可以指向不只一個檔案，而這些檔案裡的任意一個都可以是模組，被同一個
目標裡的另一個檔案匯入。這一節接下來不需要再改動這份清單——只需要改動
`src/` 目錄下的 `.scpp` 檔案。用下面的指令建置並執行每一個版本：

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## 只有被匯出的宣告才能在模組外部可見

`src/mathlib.scpp`：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

輸出：

```text
25
```

`square` 是一個普通函式：沒有 `export`，也沒有外層命名空間。`sum_of_squares`
則在 `namespace mathlib` 內部被 `export`，與模組自身的名字相符，所以
`main.scpp` 才能以 `mathlib::sum_of_squares` 的形式存取它。`square` 本身從未
跨出模組邊界——`sum_of_squares` 仍然可以在內部直接呼叫它，因為這條規則只關心
*匯入方*能看到什麼，並不關心模組自己的程式碼內部能用什麼。

直接從匯入方這邊存取 `square`，可以驗證這一點：

```cpp
import std;
import mathlib;

int main() {
    return square(5);
}
```

編譯器輸出：

```text
src/main.scpp:5:12: error: call to unknown function 'square'
 5 |     return square(5);
   |            ^
```

從 `main.scpp` 的角度看，`square` 根本就沒有被宣告過。這並不是一個存取控制上
的錯誤——這個名字壓根就沒有進入這個檔案的作用域，就好像它從來沒被寫出來過一
樣。

## 匯出的宣告必須位於和模組名相符的命名空間裡

上面單靠 `export` 還不夠。`sum_of_squares` 還必須被宣告在 `namespace mathlib
{ ... }` 內部——一個和模組自身名字相符的命名空間。`export` 和「位於所要求的命
名空間內」是兩個彼此獨立、缺一不可的條件。

（模組自己的名字可以有好幾段以點分隔的部分，比如 `mathlib.trig`；每一段都會一
一對應到一段以 `::` 分隔的命名空間。這一節裡的每個模組都只用單段名字，所以它
所要求的命名空間就是這一個名字本身——下一節會講多段模組名，以及它們對應出來的
路徑。）

完全不寫命名空間會被拒絕：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

export int sum_of_squares(int a, int b) {
    return square(a) + square(b);
}
```

編譯器輸出：

```text
src/mathlib.scpp:7:8: error: exported function 'sum_of_squares' must be declared inside a namespace -- ch11 §11.5
 7 | export int sum_of_squares(int a, int b) {
   |        ^
```

在無關的命名空間裡匯出，同樣會被拒絕：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace geometry {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

編譯器輸出：

```text
src/mathlib.scpp:8:12: error: exported function 'geometry::sum_of_squares' must be declared in namespace matching module 'mathlib' -- ch11 §11.5
 8 |     export int sum_of_squares(int a, int b) {
   |            ^
```

`geometry` 和這個模組自己的名字 `mathlib` 毫無關係，所以它會被拒絕，原因與上
面缺少命名空間的情況完全一樣。

## 比模組自身名字巢狀得更深也沒問題

命名空間的要求是一種字首比對，而不是精確比對。一個名為 `mathlib` 的模組，要求
它匯出的宣告位於 `namespace mathlib` 內部，或者位於比它巢狀得更深的任何命名空
間裡——比如 `mathlib::trig`。

`src/mathlib.scpp`：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

輸出：

```text
25
30
```

`mathlib::trig` 以 `mathlib` 開頭，所以滿足了這個要求，匯入方也能透過完整的巢
狀名字 `mathlib::trig::sin_deg` 存取到 `sin_deg`。一個模組可以自由使用巢狀命
名空間來組織自己的內部結構；這條規則只關心每一個匯出宣告的命名空間是否以模組
自身的名字開頭。

## 未匯出的宣告可以位於任何命名空間，或者根本不在任何命名空間裡

前兩節裡的命名空間規則，只約束*被匯出*的宣告。一個從未被匯出的宣告，可以位於
任意一個和模組名毫不相干的命名空間裡，也可以完全不在任何命名空間裡——就像最開
始那個範例裡的 `square` 一樣。

`src/mathlib.scpp`：

```cpp
export module mathlib;

namespace detail {
    int square(int x) {
        return x * x;
    }
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return detail::square(a) + detail::square(b);
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

輸出：

```text
25
```

`detail` 和 `mathlib` 沒有任何關係，但因為裡面的 `square` 從未被 `export`，這
種不匹配也就無關緊要了。`sum_of_squares` 仍然可以呼叫 `detail::square`，因為
這次呼叫發生在模組自己的檔案內部。匯入方卻做不到：

```cpp
import std;
import mathlib;

int main() {
    return detail::square(5);
}
```

編譯器輸出：

```text
src/main.scpp:5:12: error: call to unknown function 'detail::square'
 5 |     return detail::square(5);
   |            ^
```

即便加上 `detail::` 限定也無濟於事——`main.scpp` 從一開始就沒有把
`detail::square` 引入自己的作用域，因為它從未被匯出過。

## 普通的 `import` 是私有且不可傳遞的

到目前為止，每一個匯入方都是直接跟宣告所需內容的那個模組打交道。真正的專案往
往會把模組串聯起來：一個模組匯入另一個模組來建構自己的功能，再把自己的一部分
功能暴露給第三個檔案。第三個檔案到底能看到什麼，完全取決於中間這個模組是怎麼
匯入它自己依賴的東西的。

再加入第二個模組 `stats`，它以普通方式匯入 `mathlib`，並在內部使用它：

`src/mathlib.scpp`（恢復成第一節裡那個最簡單的版本）：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/stats.scpp`：

```cpp
export module stats;

import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`，只匯入 `stats`：

```cpp
import std;
import stats;

int main() {
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

輸出：

```text
50
```

這樣是可以的：`main.scpp` 完全沒有提到 `mathlib`，而 `sum_of_squares_twice`
也正確地在 `namespace stats` 內部被 `export`。但 `stats.scpp` 自己的
`import mathlib;` 是一次普通匯入，而普通匯入只對寫下它的那個檔案私有。它只讓
`mathlib` 的匯出內容在 `stats.scpp` 裡可見，僅此而已：

```cpp
import std;
import stats;

int main() {
    return mathlib::sum_of_squares(3, 4);
}
```

編譯器輸出：

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::sum_of_squares'
 5 |     return mathlib::sum_of_squares(3, 4);
   |            ^
```

`main.scpp` 從未匯入過 `mathlib`，而 `stats` 自己那句普通的 `import
mathlib;` 也不會把它轉發出去。只有 `stats` 自己匯出的名字——這裡就是
`sum_of_squares_twice`——才會進入 `main.scpp`。

## `export import` 會傳遞式地重新匯出

把普通的 `import` 換成 `export import`，情況就不一樣了：它會把被匯入模組匯出
的一切都重新匯出，並且是可傳遞的——傳遞給任何轉而匯入目前這個模組的檔案。

`src/stats.scpp`，改成重新匯出：

```cpp
export module stats;

export import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`，依然只匯入 `stats`：

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

輸出：

```text
25
50
```

關鍵的改動只在 `stats.scpp` 內部：`import mathlib;` 變成了 `export import
mathlib;`。`main.scpp` 自己的匯入清單沒有變化——依然只有一句 `import
stats;`——但它現在也可以直接以 `mathlib` 自己的名字呼叫
`mathlib::sum_of_squares` 了。重新匯出並不會重新命名或包裝它轉發的內容，它只
是擴大了能存取到這些內容的範圍。

## 到目前為止的私有性與可見性規則

- 一個宣告預設對自己所在的模組私有，除非有什麼東西把它匯出；
- `export` 只有在宣告位於一個和模組自身名字相符的命名空間內部時才會生效——巢
  狀得更深沒問題，位於其他任何地方都會被拒絕；
- 一個從未被匯出的宣告可以位於任意命名空間，或者根本不在任何命名空間裡，因為
  命名空間規則根本不適用於它；
- 一句普通的 `import name;` 只對寫下它的檔案私有：它讓 `name` 的匯出內容在
  這個檔案裡可見，但不會把這些內容轉發給轉而匯入這個檔案的其他檔案；
- `export import name;` 會把 `name` 自己的匯出內容以它們各自原本的限定名字，
  傳遞式地重新匯出給每一個更外層的匯入方。

這一節裡用到的每一個名字，都是一次性完整寫出的、以點或 `::` 限定的路徑。下一
節會更仔細地研究這些路徑本身：一個模組自己的點分名字是如何對應到它的命名空間
樹上的，以及在這棵樹裡從一個位置參照另一個位置的匯出內容時，應該遵循的規則。

---

[← 上一章：套件與專案清單](ch07-01-packages-and-project-manifests.md) · [目錄](README.md)
