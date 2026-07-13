# Hello, World!

現在你已經有編譯器了，接下來先讓它做一件看得見的事。

建立一個名為 `hello.scpp` 的檔案：

```cpp
import std;

int main() {
    std::println("Hello, world!");
    return 0;
}
```

如果你在上一節已經把 `scpp` 加到 `PATH` 中，可以這樣建置並執行：

```sh
scpp hello.scpp
./a.out
```

如果你仍然是在儲存庫 checkout 目錄裡直接使用剛建好的編譯器，那麼把命令改成
`./build/scpp hello.scpp` 就可以了。

輸出：

```text
Hello, world!
```

這裡用到的 `std::println` 來自 scpp 的標準函式庫，用來印出一整行文字。

這個極小程式裡，其實已經藏了幾個重要概念：

- `int main()` 是程式入口；
- `import std;` 讓這個檔案可以使用標準函式庫；
- 整個程式在表面上依然看起來像普通 C++，這正是 scpp 最核心的設計目標之一。

下一節裡，我們會讓程式依然維持同樣的小巧，但把它放進一個真正的 manifest-based 專
案裡。

---

[← 上一節：建置編譯器](ch01-01-building-the-compiler.md) · [目錄](README.md) · [下一節：第一個 project build →](ch01-03-hello-project-builds.md)
