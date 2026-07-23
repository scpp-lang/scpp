# 固定大小陣列

上一節用「把一個模組拆到多個檔案裡」結束了第 7 章。這一章把焦點從專案版面配
置拉回到普通資料本身:固定大小陣列、字元緩衝區,以及對連續儲存做借用視圖。

固定大小陣列會把一組已知數量的元素直接存放在自己內部。它自己擁有這些值,而它
的長度也是型別的一部分。當你一開始就知道自己需要多少個元素時,陣列就是 scpp
目前最簡單的序列型別。

對下面每個可以執行的例子,把檔案存成 `arrays.scpp`,然後這樣編譯執行:

```sh
scpp arrays.scpp -o arrays
./arrays
```

對於那些本來就應該被拒絕的例子,如果你想讓編譯器輸出逐字匹配,就按診斷區塊裡
給出的描述性檔名保存。

## 用固定元素個數宣告陣列

`T[N]` 的意思是「一個元素型別為 `T`、長度為 `N` 的陣列」。

```cpp
import std;

int main() {
    int scores[4]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    scores[3] = 40;

    std::println("{} {}", scores[0], scores[3]);
    return 0;
}
```

輸出:

```text
10 40
```

`scores` 直接擁有四個 `int` 值。這裡的 `[4]` 不是只給人看的註解;它本身就是變
數型別的一部分,每一個合法索引都必須落在這個固定長度裡。

空大括號也很重要。就今天的 scpp 來說,`T[N]{}` 是最穩妥的起點:先把整個陣列初
始化好,然後再把你想要的元素一個個填進去。

## 用 `auto&` 做 range-based `for` 可以原地修改每個元素

第 3.5 節已經用過 range-based `for` 來依次讀取陣列元素。把迴圈變數寫成
`auto&` 時,拿到的就是每個元素的可變參照。

```cpp
import std;

int main() {
    int values[3]{};
    int next = 1;

    for (auto& value : values) {
        value = next * 10;
        next = next + 1;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

輸出:

```text
10
20
30
```

第一層迴圈裡的每個 `value` 都參照著陣列裡的真實元素,不是一份拷貝。透過這個
參照寫入,改到的就是陣列自己。

## 陣列長度也可以來自前面定義的 `constexpr`

長度不一定非得手寫成字面量。只要是前面已經可用、而且能解析成正整數長度的常量
運算式,也一樣可以。

```cpp
import std;

int main() {
    constexpr int count = 4;
    int values[count]{};
    values[3] = 9;

    std::println("{} {}", count, values[3]);
    return 0;
}
```

輸出:

```text
4 9
```

這樣通常比到處重複同一個字面量更清楚。關鍵不在於這個名字剛好叫 `count`,而在
於它是個 `constexpr`,所以編譯器能在程式執行前就把陣列長度算出來。

## 陣列長度必須是常量運算式

普通的執行期變數不能拿來當陣列長度。

```cpp
int main() {
    int count = 4;
    int values[count]{};
    return 0;
}
```

編譯器輸出:

```text
array_nonconst_bound_fail.scpp: error: 3:16: expression is not a constant expression: identifier 'count' is not available
```

這裡的 scpp 不支援「執行期才知道長度的區域陣列」。編譯器必須一開始就知道
`values` 的完整大小,所以這個長度必須在編譯階段就能確定,不能等到執行時才發
現。

## 明顯越界的常量索引會被立刻拒絕

因為長度本來就是型別的一部分,所以只要一個常量索引明顯不可能合法,編譯器就能
直接在編譯期拒絕它。

```cpp
int main() {
    int values[3]{};
    return values[3];
}
```

編譯器輸出:

```text
array_out_of_bounds_compile.scpp:3:12: error: array subscript 3 is out of bounds for array of size 3
 3 |     return values[3];
   |            ^
```

對 `int values[3]` 來說,合法索引只有 `0`、`1`、`2`。既然原始碼裡直接寫出了 `3`,
編譯器就已經有足夠的資訊在程式執行前把它攔下來。

## 今天請按元素逐個填值,不要直接寫一個值列表

在普通 C++ 裡,`int scores[4]{10, 20, 30, 40};` 會是第一節那個例子的自然寫法。
但在今天的 scpp 裡,這種「多個元素一起寫在大括號裡」的初始化還不支援。

```cpp
int main() {
    int scores[4]{10, 20, 30, 40};
    return 0;
}
```

編譯器輸出:

```text
array_brace_init.scpp:2:5: error: brace-initialization of this member requires exactly one expression
 2 |     int scores[4]{10, 20, 30, 40};
   |     ^
```

這也就是為什麼本節所有可執行示例都先從 `T[N]{}` 開始,再把元素一個個指定進去。

## 到目前為止,固定大小陣列的規則是

- `T[N]` 宣告「恰好有 `N` 個 `T` 元素」的陣列;
- 陣列直接擁有這些元素,而 `N` 本身就是型別的一部分;
- 用 `auto&` 做 range-based `for` 可以原地修改每個元素;
- 長度可以來自前面定義的 `constexpr`,但不能來自普通執行期變數;
- 明顯越界的常量索引會在編譯期被拒絕;
- 今天最穩妥的寫法是先用 `{}` 做零初始化,再明確填每個元素。

下一節仍然會繼續講陣列,不過會把元素型別收窄到 `char`,把這塊固定大小儲存當成
一個跟 C 相容的文字緩衝區來使用。

---

[← 上一章：把模組拆到不同檔案中](ch07-05-separating-modules-into-different-files.md) · [目錄](README.md)
