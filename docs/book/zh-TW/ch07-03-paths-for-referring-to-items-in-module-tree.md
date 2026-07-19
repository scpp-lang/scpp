# 在模組樹中參照項目的路徑

上一節裡,每一個項目都是透過路徑來存取的:`mathlib::sum_of_squares`、
`mathlib::trig::sin_deg`、`stats::sum_of_squares_twice`。每一次都是在呼叫的地
方完整地寫出來。這一節要看看路徑到底是什麼,模組自己的點分名字如何決定路徑的
形狀,以及 scpp 在跳過其中任何一部分這件事上,到底能提供多少捷徑。

一個路徑不過是一個項目的命名空間巢狀層級,以 `::` 連接起來,最後加上它自己的
名字。它和宣告這個項目的檔案是哪一個毫無關係,而且它和模組自己的點分匯入名字
是兩回事——下面第一節會說明為什麼這兩者用了不同的分隔符號。

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

## 模組的點分名字,按段對應到路徑

[用模組控制作用域與可見性](ch07-02-control-scope-and-privacy-with-modules.md)
曾經順帶提到,模組自己的名字可以有好幾段以點分隔的部分,比如
`mathlib.trig`;每一段都會一一對應到一段以 `::` 分隔的命名空間。到目前為止的
每個模組都只用單段名字,這一點從來沒有真正用到過。用一個真正的兩段式模組名
字,可以把它坐實。

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

輸出:

```text
30
```

`import mathlib.trig;` 用點來為模組命名,和模組自己為自己命名的方式一樣。存
取它匯出的內容仍然用 `::`,和之前完全一樣——`mathlib::trig::sin_deg`。模組自
己的兩段以點分隔的部分,`mathlib` 和 `trig`,變成了兩段必須相符的 `::` 分隔命
名空間,`mathlib` 和 `trig`。這仍然是上一節那條命名空間相符規則,只是模組自己
的名字現在有不只一段需要相符。

這條規則依然完全成立:比模組自身名字少一段的命名空間會被拒絕,就像之前完全
沒有命名空間會被拒絕一樣。

```cpp
export module mathlib.trig;

namespace mathlib {
    export int sin_deg(int x) {
        return x;
    }
}
```

編譯器輸出:

```text
src/trig.scpp:4:12: error: exported function 'mathlib::sin_deg' must be declared in namespace matching module 'mathlib.trig' -- ch11 §11.5
 4 |     export int sin_deg(int x) {
   |            ^
```

`namespace mathlib` 只提供了兩段裡的第一段,所以它會被當成任何一個不相符的命
名空間來處理:和無關命名空間、或者完全沒有命名空間,是同一種錯誤。

## 只有宣告在同一個命名空間裡的名字才能省略路徑

一個命名空間仍然可以比模組自身名字巢狀得更深——這部分規則和上一節完全一
樣,沒有變化。這裡真正新出現的,是一次不帶路徑的呼叫到底能存取到什麼,一旦命
名空間開始巢狀起來。

`src/trig.scpp`,加上一個輔助函式和一個巢狀更深的命名空間:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    int double_it(int x) {
        return x * 2;
    }

    export int sin_deg(int x) {
        return double_it(x);
    }
}

namespace mathlib::trig::deg {
    export int right_angle() {
        return mathlib::trig::double_it(45);
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(400));
    std::println("{}", mathlib::trig::deg::right_angle());
    return 0;
}
```

輸出:

```text
800
90
```

`sin_deg` 完全不帶路徑地呼叫 `double_it`,這樣是可以的,因為兩者都直接宣告在
同一個 `namespace mathlib::trig { ... }` 區塊裡。`right_angle` 宣告在更深一層
的 `mathlib::trig::deg` 裡,它透過完整路徑 `mathlib::trig::double_it` 呼叫同
一個 `double_it`——即使 `mathlib::trig` 就是它自己直接外層的命名空間。拿掉這
個路徑,直接在 `mathlib::trig::deg` 裡面裸呼叫 `double_it`,是行不通的:

```cpp
namespace mathlib::trig::deg {
    export int right_angle() {
        return double_it(45);
    }
}
```

編譯器輸出:

```text
src/trig.scpp:15:16: error: call to unknown function 'double_it'
 15 |         return double_it(45);
    |                ^
```

一次不帶路徑的呼叫,只能存取到直接宣告在同一個命名空間裡的名字(或者像之前每
個範例裡的普通函式那樣,根本不在任何命名空間裡的名字)。它不會像查找巢狀程式
碼區塊裡的變數那樣,向外一層一層爬過命名空間。要跨到任何其他命名空間——哪怕
只是直接包住目前這個命名空間的那一層——都必須寫出完整路徑。

## 開頭的 `::` 會從最外層作用域開始查找

一個路徑也可以以 `::` 開頭。這不會改變一個像 `mathlib::trig::sin_deg` 這樣完
整寫出的路徑所存取到的內容——它存取到的仍然是同一個項目——但它能保證查找是從
最外層作用域開始的,而不會考慮呼叫處已經在作用域裡的其他任何東西。這個差異只
有在呼叫處的裸名字本來就可能存取到別的東西時,才會顯現出來。

```cpp
import std;

int count() {
    return 100;
}

int main() {
    int count = 7;
    std::println("{}", count);
    std::println("{}", ::count());
    return 0;
}
```

輸出:

```text
7
100
```

兩個 `count` 在 `main` 內部都是可見的:一個區域變數,還有一個同名的普通函
式——如果沒有開頭的 `::`,後者本來就會被區域變數遮蔽掉。裸寫 `count` 存取到
的是區域變數。寫 `::count()` 則會直接跳過它,存取到宣告在最外層作用域的那個
函式。同樣,開頭的 `::` 放在一個完整路徑前面,意思也一樣——從最外層作用域開
始,然後嚴格按照寫出來的路徑往下找:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", ::mathlib::trig::sin_deg(400));
    std::println("{}", ::mathlib::trig::deg::right_angle());
    return 0;
}
```

輸出:

```text
800
90
```

這裡 `main` 的呼叫處,作用域裡沒有任何東西可能和 `mathlib::trig::sin_deg` 或
者 `mathlib::trig::deg::right_angle` 混淆,所以開頭的 `::` 對結果沒有任何影
響。它對任何路徑都是可用的,不只是那些真正需要它的路徑。

## 路徑依然無法存取從未被匯出的內容

以上這些都沒有重新打開上一節的私有性規則。一個和某個宣告的命名空間巢狀完全
相符的路徑,如果這個宣告從未被 `export`,依然會失敗——單單把路徑寫對是不夠
的。

```cpp
import std;
import mathlib.trig;

int main() {
    return mathlib::trig::double_it(5);
}
```

編譯器輸出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::double_it'
 5 |     return mathlib::trig::double_it(5);
   |            ^
```

`double_it` 確實就位於 `mathlib::trig::double_it`——這正是上面 `trig.scpp` 內
部自己用來呼叫它的那個路徑——但它從未被匯出,所以沒有任何路徑能從
`main.scpp` 存取到它。路徑只描述一個東西位於哪裡;它能不能從模組外部沿著這條
路徑被找到,依然完全取決於 `export`。

## 到目前為止的路徑規則

- 一個路徑就是一個項目的命名空間巢狀層級,以 `::` 連接起來,最後加上它自己的
  名字——和宣告它的檔案是哪一個無關;
- 模組自己的點分名字,按段對應到它匯出內容必須位於的 `::` 分隔命名空間,不管
  它有多少段;
- 一次不帶路徑的呼叫,只能存取到宣告在同一個命名空間裡的名字,或者根本不在
  任何命名空間裡的名字——存取任何其他命名空間,包括直接包住呼叫處的那一層,
  都需要完整路徑;
- 開頭的 `::` 會讓一個路徑從最外層作用域開始查找,先於呼叫處已經在作用域裡的
  任何其他東西;
- 一個路徑始終只能存取到位置正確並且被 `export` 過的宣告——把路徑寫對本身,
  並不能跨越上一節裡的那道私有性邊界。

這一節裡的每一個路徑,依然是在每一個需要它的檔案裡手動完整寫出來的。下一節
回到 `import` 本身,看看它和一個路徑自己的限定名稱,在實際使用中是如何配合
的。

---

[← 上一章：用模組控制作用域與可見性](ch07-02-control-scope-and-privacy-with-modules.md) · [目錄](README.md)
