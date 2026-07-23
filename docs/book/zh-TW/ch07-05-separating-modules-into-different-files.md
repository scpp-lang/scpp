# 把模組拆到不同檔案中

到目前為止,一個模組始終對應著恰好一個檔案。真實程式很少一直這麼小。一旦一
個模組自己的原始碼需要分散到多個檔案裡,scpp 仍然只保留一個主介面單元,再讓
這個模組的其餘檔案以 partition 的形式附著到它上面。

在今天普通的 `scpp build` project build 裡,把一個模組拆成多個檔案的受支援
方式,就是:一個主介面單元,再加上一個或多個 partition。

下面的每個例子都放在同一個 package 裡。

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

## `export import :part;` 會重新匯出一個 interface partition

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :trig;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/trig.scpp`:

```cpp
export module mathlib:trig;

namespace mathlib {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", mathlib::sin_deg(30));
    return 0;
}
```

輸出:

```text
25
30
```

`mathlib.scpp` 依然是這個模組的主介面單元:它是那個寫著 `export module
mathlib;` 的檔案。`trig.scpp` 也屬於同一個模組,但它用
`export module mathlib:trig;` 給這個模組命名了一個 partition。主介面單元
再用 `export import :trig;` 把它重新匯出,所以模組外部的檔案依然只寫
`import mathlib;`。模組外部不會直接 import `:trig`。

## partition 名字不會變成路徑裡的另一個 `::` 段

上面的 partition 叫 `:trig`,但這並不會讓 `trig` 變成任何匯出項目路徑的一部
分。

`src/main.scpp`:

```cpp
import mathlib;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

編譯器輸出:

```text
src/main.scpp:4:12: error: call to unknown function 'mathlib::trig::sin_deg'
 4 |     return mathlib::trig::sin_deg(30);
   |            ^
```

`sin_deg` 是從 `namespace mathlib` 裡匯出的,所以它的路徑是
`mathlib::sin_deg`。partition 名字 `trig` 只是幫助組織這個模組自己的原始檔,
不會新建一個 namespace 段,也不會改動任何限定名稱。跟上一節一樣,路徑依然只
來自宣告本身的 namespace 巢狀結構。

## `import :part;` 會把 partition 留在模組內部

partition 也可以只為了模組內部使用而被 import。

`src/mathlib.scpp`:

```cpp
export module mathlib;

import :detail;

namespace mathlib {
    export int doubled_sum(int a, int b) {
        return double_it(a + b);
    }
}
```

`src/detail.scpp`:

```cpp
module mathlib:detail;

namespace mathlib {
    int double_it(int x) {
        return x * 2;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::doubled_sum(3, 4));
    return 0;
}
```

輸出:

```text
14
```

這裡的 `detail.scpp` 是一個 implementation partition:
`module mathlib:detail;` 這行自己的模組宣告上沒有 `export`。主介面單元用
普通的 `import :detail;` 在內部接上它,這就足夠讓 `doubled_sum` 呼叫
`double_it` 了。

但 importer 依然不能直接碰到 `double_it`:

```cpp
import std;
import mathlib;

int main() {
    return mathlib::double_it(5);
}
```

編譯器輸出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::double_it'
 5 |     return mathlib::double_it(5);
   |            ^
```

普通的 `import :detail;` 只是在把另一個檔案接進同一個模組自己的實作裡。它不
會讓這個 partition 變成模組公開介面的一部分。

## 只有模組自己的檔案才能 import 一個 partition

模組外部的檔案仍然不能在 `import` 後面寫 partition 名字。

`src/main.scpp`:

```cpp
import mathlib:trig;

int main() {
    return 0;
}
```

編譯器輸出:

```text
src/main.scpp:1:15: error: expected ';' but found ':'
 1 | import mathlib:trig;
   |               ^
```

對模組外部來說,`import` 依然是在命名一個完整模組,這一點跟上一節完全一樣。
`:trig` 這種寫法,只給那些已經屬於 `mathlib`,還要再 import 這個模組另一部
分的檔案使用。外部程式碼 import 的仍然是整個模組,拿到的是主介面單元選擇重
新匯出的那些內容。

## `export import` 只能用在 interface partition 上

implementation partition 從構造上就是內部細節,所以試圖重新匯出它會被拒絕。

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :detail;
```

編譯器輸出:

```text
src/mathlib.scpp:3:8: error: cannot 'export import' partition 'mathlib:detail': it is an implementation partition ('module ...;' with no 'export' on its own module declaration), so it can never export anything to the outside (ch11 §11.4)
 3 | export import :detail;
   |        ^
```

只有 interface partition -- 也就是那些寫著 `export module name:part;` 的檔
案 -- 才能被重新匯出。implementation partition 可以貢獻只在模組內部使用的
程式碼,但它永遠不能進入模組的匯出面。

## 一個模組、一個主介面、多個檔案

- 依然只有一個檔案宣告 `export module mathlib;` -- 這就是主介面單元;
- 額外檔案透過 `module mathlib:part;` 或 `export module mathlib:part;`
  加入同一個模組;
- 模組外部檔案依然寫 `import mathlib;`,不會直接寫 partition 名字;
- partition 名字是組織原始檔用的,不是限定路徑的一部分;
- `export import :part;` 會重新匯出一個 interface partition,而普通的
  `import :part;` 則把 partition 留在模組內部。

有了 partition,一個模組就可以長到跨越多個檔案,而不需要推翻前幾節已經講過
的任何規則:可見性仍然由 `export` 決定,路徑仍然由 namespace 決定,`import`
仍然帶來的是整個模組,而不是單獨某個項目。

---

[← 上一章：使用 `import` 與限定名稱](ch07-04-using-import-and-qualified-names.md) · [目錄](README.md) · [下一章：固定大小陣列 →](ch08-01-fixed-size-arrays.md)
