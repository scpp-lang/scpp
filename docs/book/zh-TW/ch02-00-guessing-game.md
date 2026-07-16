# 做一個猜數字小遊戲

上一章裡，你已經證明工具鏈能正常運作了。現在該寫一個更像樣一點的程式了。

這一章我們來做一個小小的猜數字遊戲。程式會在 1 到 100 之間選一個秘密數字，玩家
持續輸入猜測，程式則回答：

- 如果猜得比秘密數字小，就輸出 “Too small!”
- 如果猜得比秘密數字大，就輸出 “Too big!”
- 如果猜對了，就輸出 “You win!”

這一章會先用起來，後面再細講幾個 scpp 裡的基礎概念：

- 可變的區域變數，
- `while` 迴圈，
- `if` 條件分支，
- 檢查 `std::expected` 結果，
- 從 `scpp` 模組匯入輔助 API，
- 以及用 `std::from_chars` 解析使用者輸入。

## 準備這個專案

建立一個目錄，然後準備好頂層的 `scpp.toml`，以及放在 `src/` 底下的 `main.scpp`。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "guessing_game"
version = "0.1.0"

[[bin]]
name = "guessing-game"
sources = ["src/**/*.scpp"]
```

## 完整程式

`src/main.scpp`：

```cpp
import std;
import scpp;

int main() {
    std::println("Guess the number!");

    int secret_number = scpp::rand::uniform_int_rand(100) + 1;

    while (true) {
        std::println("Please input your guess.");

        auto line_result = scpp::io::getline();
        if (!line_result.has_value()) {
            std::println("Input closed.");
            return 1;
        }
        const std::string& line = line_result.value();
        int guess = 0;
        auto parse_result = std::from_chars(line.c_str(), line.c_str() + line.size(), guess);
        bool parse_failed = static_cast<int>(parse_result.ec) != 0;
        if (parse_failed || parse_result.ptr != line.c_str() + line.size()) {
            std::println("Please enter a whole number between 1 and 100.");
            continue;
        }

        if (guess < 1 || guess > 100) {
            std::println("Please enter a whole number between 1 and 100.");
            continue;
        }

        if (guess < secret_number) {
            std::println("Too small!");
            continue;
        }
        if (guess > secret_number) {
            std::println("Too big!");
            continue;
        }

        std::println("You win!");
        break;
    }

    return 0;
}
```

在那個目錄裡建置並執行它：

```sh
scpp build
./.scpp/build/*/dev/guessing_game/guessing-game
```

因為秘密數字是隨機產生的，所以每次執行時的完整對話都不一樣。一輪執行裡的輸出可
能像這樣：

```text
Guess the number!
Please input your guess.
Too small!
Please input your guess.
Too big!
Please input your guess.
You win!
```

## 這一章其實已經引入了什麼

這個小程式裡，其實已經塞進不少東西。

`scpp::rand::uniform_int_rand(100) + 1` 會替我們產生一個 1 到 100 之間的新秘密數
字。這個輔助函式把隨機數相關的準備工作都收在後面，因此這個第一個互動式程式可以
把重點繼續放在控制流程和輸入處理上。

每一輪裡我們都會呼叫 `scpp::io::getline()`。它同樣回傳 `std::expected`。成功時，
我們拿到一個真正的 `std::string`；如果輸入結束或讀取失敗，就印出一條訊息並乾淨
地結束。

接著我們用 `std::from_chars` 把這段字串解析成整數。它會直接從字串的字元緩衝區讀
取十進位數字，把結果寫進 `guess`，並透過回傳結果裡的 `ec` 欄位回報問題。這裡我
們把任何非零的 `ec` 都當成解析失敗；同時也會檢查回傳的指標是否真的走到整行末
尾，因此像 `12abc` 這樣的輸入也會被拒絕，而不是悄悄只解析前半段。

`while (true)` 會讓遊戲一直執行，直到某一次猜測正確為止。後面的 `if` 分支鏈則決
定這次應該印出哪一條提示訊息。

重要的是：你現在已經寫出一個真正的互動式程式。它會讀取輸入、保存狀態、重複執行
工作，並根據條件選擇不同分支。下一章我們會放慢節奏，把這些基礎構件逐一拆開來講
清楚。

---

[← 上一章：第一個 project build](ch01-03-hello-project-builds.md) · [目錄](README.md) · [下一章：變數與明確初始化 →](ch03-01-variables-and-explicit-initialization.md)
