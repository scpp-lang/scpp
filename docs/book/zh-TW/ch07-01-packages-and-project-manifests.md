# 套件與專案清單

目前為止，你寫的每個 scpp 程式都只放在一個檔案裡，或者就是你在 [第一個 project
build](ch01-03-hello-project-builds.md) 裡建置過的那種小型、以清單為基礎的專案。
這一章要往前走一步：一個真正的 scpp 專案是怎麼組織的，一個專案怎麼產出不只一
個執行檔，以及這些執行檔之間又怎麼共享程式碼。

這一章接下來會反覆用到兩個詞，它們說的不是同一件事：

- **套件**是一個根目錄下有 `scpp.toml` 清單的目錄，是 `scpp build` 操作的基本
  單位。
- **模組**是由 `export module` / `module` 引入、由 `import` 使用的編譯單元；編
  譯器在檢查「從哪裡能看到什麼」時，正是以模組為單位來推理的。

一個套件的清單可以宣告任意數量的執行檔目標，也可以宣告任意數量的函式庫。這些函
式庫和這些執行檔之間要靠模組才能真正共享宣告。這一節始終停留在套件這一層；下一
節會深入到模組內部。

對於下面的每一個專案，先建立好展示的檔案，然後在該專案自己的目錄裡執行：

```sh
scpp build
```

執行檔會落在 `.scpp/build/<target triple>/dev/<package name>/<binary name>` 下
面，和 [第一個 project build](ch01-03-hello-project-builds.md) 裡完全一樣。

## 一個套件需要一份清單和至少一個目標

光有 `manifest-version = 1`，再加上一個帶 `name` 和 `version` 的 `[package]`
表，還不夠。清單還需要至少一個建置目標：一個 `[[bin]]` 表、一個 `[[lib]]` 表，
或者兩者都有。

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"
```

```sh
scpp build
```

編譯器輸出：

```text
error: manifest must declare at least one [[lib]] or [[bin]] target
```

這一節接下來會在同一份清單上逐個新增目標。

## `sources` 是你自己寫的 glob 模式，不是固定檔名

一個 `[[bin]]` 表需要 `name` 和 `sources` 兩個欄位。`name` 同時決定了執行檔自
己的檔名，以及之後 `--bin` 用來選取它時所用的名字。`sources` 是一組 glob 模式，
會針對套件自己的目錄樹展開——`*` 只在一層目錄內匹配，`**` 可以跨目錄匹配。
scpp 不會為執行檔的入口保留任何固定檔名；具體的目錄版面配置完全由你透過
`sources` 自己決定。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["*.scpp"]
```

`greeter.scpp`：

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
```

輸出：

```text
Hello, scpp!
```

這裡 `sources = ["*.scpp"]` 會匹配套件根目錄下所有直接存在的 `.scpp` 檔案——目
前還沒有 `src/` 目錄，也不是必須要有。

## 一個套件可以建置出不只一個執行檔

一份清單可以宣告任意數量的 `[[bin]]` 表。每一個都會根據自己指定的原始碼獨立編
譯、連結，互不影響。當專案的檔案不只一個之後，把原始碼放到 `src/` 底下，可以讓
每個目標的 glob 模式精確指向自己名下的那些檔案。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greeter.scpp`：

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

`src/shout.scpp`：

```cpp
import std;

int main() {
    std::println("HELLO, SCPP!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

輸出：

```text
Hello, scpp!
HELLO, SCPP!
```

直接執行 `scpp build` 會建置出兩個執行檔。如果只想建置其中一個，需要明確點名：

```sh
scpp build --bin shout
./.scpp/build/*/dev/greeter/shout
```

輸出：

```text
HELLO, SCPP!
```

`[[bin]]` 的名字在同一個套件裡也必須是唯一的。如果兩個 `[[bin]]` 表都叫
`"greeter"`，會在兩者都還沒編譯之前就被拒絕：

```text
error: duplicate [[bin]] target name 'greeter'
```

## 函式庫目標讓執行檔之間自動共享程式碼

`[[lib]]` 表和 `[[bin]]` 很像——同樣是 `name` 和 `sources` 兩個欄位——但它編譯出
來的是一個模組，而不是連結出一個可執行檔。在這個模組的原始碼裡，`export module
greetings;` 為模組命名，與之對應的 `namespace greetings { ... }` 則標出哪些宣
告會被 `export` 暴露給匯入方。

同一個套件裡的每個 `[[bin]]` 目標都可以直接用名字 `import` 這個模組，不需要任
何額外的參數：`scpp build` 在同一次執行裡早些時候就已經把它編譯好了，清單也記
錄了它的介面和封存檔放在哪裡，供後續建置使用。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greetings.scpp`：

```cpp
export module greetings;

namespace greetings {
    export const char* phrase(bool shout) {
        return shout ? "HELLO, SCPP!" : "Hello, scpp!";
    }
}
```

`src/greeter.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(false));
    return 0;
}
```

`src/shout.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

輸出：

```text
Hello, scpp!
HELLO, SCPP!
```

輸出和之前一樣，但現在 `greeter` 和 `shout` 共享同一份實作，而不是在每個檔案裡
各自重複一遍這句話。建置過程還會在兩個執行檔旁邊，留下函式庫自己編譯出的產物：

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
```

`--lib` 只建置函式庫本身，不會連結任何一個執行檔：

```sh
scpp build --lib
```

## 一個套件也可以建置出不只一個函式庫

一份清單可以宣告任意數量的 `[[lib]]` 表，就像它可以宣告任意數量的 `[[bin]]` 表
一樣。套件裡的每個 `[[bin]]` 目標都可以 `import` 其中任意一個，不只是它恰好需
要的那一個——上面只用了一個 `[[lib]]` 表，並沒有什麼特殊之處。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[lib]]
name = "farewells"
sources = ["src/farewells.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

這裡新增了第二個函式庫 `farewells`，和已有的 `greetings` 放在一起。

`src/farewells.scpp`：

```cpp
export module farewells;

namespace farewells {
    export const char* phrase() {
        return "Goodbye, scpp!";
    }
}
```

`greeter` 現在匯入了兩個函式庫——不需要任何清單選項來開啟這一點，只要 `import`
就夠了。

`src/greeter.scpp`：

```cpp
import std;
import greetings;
import farewells;

int main() {
    std::println("{}", greetings::phrase(false));
    std::println("{}", farewells::phrase());
    return 0;
}
```

`shout` 沒有變化，仍然只匯入 `greetings`——一個 `[[bin]]` 目標可以自由匯入套件
裡 `[[lib]]` 目標的任意子集，不必匯入全部。

`src/shout.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

輸出：

```text
Hello, scpp!
Goodbye, scpp!
HELLO, SCPP!
```

建置過程現在會把兩個函式庫各自的產物並排留下：

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/modules/farewells.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
.scpp/build/*/dev/greeter/archives/libfarewells.scppa
```

單獨使用 `--lib` 仍然會建置每一個函式庫目標，不連結任何執行檔，和只有一個函式
庫時一樣。要在多個函式庫中只建置某一個，就像 `--bin` 選擇某個執行檔一樣，點名
它即可：

```sh
scpp build --lib farewells
```

這樣只會建置 `farewells`；`greetings` 和兩個執行檔都不受影響。

`[[lib]]` 的名字在同一個套件裡也必須唯一，和 `[[bin]]` 的名字一樣。如果兩個
`[[lib]]` 表都叫 `"greetings"`，也會被同樣地拒絕：

```text
error: duplicate [[lib]] target name 'greetings'
```

## 目前為止的清單規則

- 套件是一個根目錄下有 `scpp.toml` 的目錄；
- 清單需要 `manifest-version = 1`、一個 `[package]` 表，以及至少一個 `[[bin]]`
  或 `[[lib]]` 表；
- `sources` 是你自己寫的一組 glob 模式，不是固定的檔名；
- 一個套件可以建置任意數量的執行檔，用 `--bin <name>` 單獨選擇要建置哪一個；
- 一個套件可以建置任意數量的函式庫，用 `--lib <name>` 單獨選擇要建置哪一個，或
  者單獨用 `--lib` 一次建置全部；
- 套件裡的每個 `[[bin]]` 目標都會自動看到這個套件所有的 `[[lib]]` 模組，不需要
  任何額外的 `--import` 參數。

套件講的是建置層面的故事。下一節會轉到語言層面：模組如何用命名空間，在這些檔案
內部和之間控制作用域與可見性。

---

[← 上一章：如何把「信任」局部化到真實程式裡](ch06-03-localizing-trust-in-real-programs.md) · [目錄](README.md)
