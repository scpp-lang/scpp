# 建置編譯器

scpp 目前是從原始碼建置的。如果你已經把儲存庫抓到本機，第一步就是先產出一個可用
的 `scpp` 執行檔。

## 你需要準備什麼

從原始碼建置時，請先準備：

- CMake 3.28 或更新版本
- Ninja
- Clang/LLVM 22
- SQLite 開發標頭檔與函式庫
- zstd 開發標頭檔與函式庫

在 Debian 或 Ubuntu 上，可以這樣安裝：

```sh
sudo apt install clang cmake ninja-build llvm-22-dev libsqlite3-dev libzstd-dev
```

## 設定並建置

在儲存庫根目錄執行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build
```

命令完成後，剛建好的編譯器會在這裡：

```text
./build/scpp
```

## 可選：把它安裝到你的 `PATH` 中

如果你希望這一章後面的命令可以直接寫 `scpp`，而不是每次都寫 `./build/scpp`，可以
把建置結果安裝到你自己控制的前綴目錄裡：

```sh
cmake --install build --prefix "$HOME/.local/scpp"
export PATH="$HOME/.local/scpp/bin:$PATH"
```

這個安裝步驟會建立一棵自包含的目錄樹，裡面同時包含編譯器本體，以及它需要的
stdlib 檔案。

如果你暫時不想安裝，也完全沒問題。你仍然可以繼續在儲存庫根目錄直接使用
`./build/scpp`。

## 到這裡你已經有了什麼

到這裡，你已經有一個真正可用的編譯器執行檔了。下一節，我們就拿它來建置最小的
scpp 程式。

---

[← 上一節：開始使用](ch01-00-getting-started.md) · [目錄](README.md) · [下一節：Hello, World! →](ch01-02-hello-world.md)
