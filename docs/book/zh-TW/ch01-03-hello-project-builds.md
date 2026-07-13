# 第一個 project build

單檔編譯很適合快速試驗。但只要你想要一個有名字的執行檔、以及一個真正的專案目
錄，scpp 的 manifest-based build 模式就會更順手。

建立一個目錄，然後準備好頂層的 `scpp.toml`，以及放在 `src/` 底下的 `main.scpp`。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "starter"
version = "0.1.0"

[[bin]]
name = "hello"
root = "src/main.scpp"
sources = ["src/**/*.scpp"]
```

`src/main.scpp`：

```cpp
import std;

int main() {
    std::println("Hello from a project build!");
    return 0;
}
```

在這個目錄裡執行：

```sh
scpp build
./.scpp/build/*/dev/starter/hello
```

輸出：

```text
Hello from a project build!
```

輸出路徑裡的 `*` 會展開成你的 target triple，例如 `x86_64-pc-linux-gnu`。scpp 會
把建置產物放到 `.scpp/build/` 底下，因此專案目錄本身就能維持得比較小，也比較可預
期。

到這裡，第一章的目標就完成了：

- 你建置了編譯器；
- 你編譯了一個單檔程式；
- 你建置了一個帶 manifest、帶具名執行檔目標的專案。

下一章會繼續保持「動手做」的節奏，但重點會從「把工具跑起來」切換成「寫一個稍微
更完整一點的程式」：一個會讀取輸入、產生秘密數字，並把變數、迴圈和條件組合起來
使用的猜數字遊戲。

---

[← 上一節：Hello, World!](ch01-02-hello-world.md) · [目錄](README.md) · [下一節：做一個猜數字小遊戲 →](ch02-00-guessing-game.md)
