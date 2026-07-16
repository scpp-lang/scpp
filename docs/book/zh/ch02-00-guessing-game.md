# 做一个猜数字小游戏

上一章里，你已经证明了工具链是能工作的。现在该写一个稍微更像样一点的程序了。

这一章我们来做一个很小的猜数字游戏。程序会在 1 到 100 之间选一个秘密数字，玩家
不断输入猜测，程序则回答：

- 如果猜得比秘密数字小，就输出 “Too small!”
- 如果猜得比秘密数字大，就输出 “Too big!”
- 如果猜对了，就输出 “You win!”

这一章会先用起来、后面再细讲几个 scpp 里的基础概念：

- 可变的局部变量，
- `while` 循环，
- `if` 条件分支，
- 检查 `std::expected` 结果，
- 从 `scpp` 模块导入辅助 API，
- 以及用 `std::from_chars` 解析用户输入。

## 准备这个项目

创建一个目录，然后把顶层的 `scpp.toml` 和放在 `src/` 下面的 `main.scpp` 准备好。

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

## 完整程序

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

在那个目录里构建并运行它：

```sh
scpp build
./.scpp/build/*/dev/guessing_game/guessing-game
```

因为秘密数字是随机生成的，所以每次运行时的完整对话都不一样。一轮运行里的输出可
能像这样：

```text
Guess the number!
Please input your guess.
Too small!
Please input your guess.
Too big!
Please input your guess.
You win!
```

## 这一章其实已经引入了什么

这个小程序里其实已经塞进了不少东西。

`scpp::rand::uniform_int_rand(100) + 1` 会给我们一个 1 到 100 之间的新秘密数
字。这个辅助函数把随机数相关的准备工作都藏在背后，因此这个第一个交互式程序可以
把重点继续放在控制流和输入处理上。

每一轮里我们都会调用 `scpp::io::getline()`。它同样返回 `std::expected`。成功时，
我们拿到一个真正的 `std::string`；如果输入结束或者读取失败，就打印一条消息并干
净地退出。

接着我们用 `std::from_chars` 把这段字符串解析成整数。它会直接从字符串的字符缓冲
区里读取十进制数字，把结果写进 `guess`，并通过返回结果里的 `ec` 字段报告问题。
这里我们把任何非零的 `ec` 都当作解析失败；同时也会检查返回的指针是否真的走到了
整行末尾，因此像 `12abc` 这样的输入也会被拒绝，而不是悄悄只解析前半段。

`while (true)` 会让游戏一直运行，直到某一次猜测正确为止。后面的 `if` 分支链则决
定这次应该打印哪一条提示消息。

重要的是：你现在已经写出了一个真实的交互式程序。它会读入输入、保存状态、重复执
行工作，并根据条件选择不同分支。下一章我们会放慢节奏，把这些基础构件逐个拆开来
讲清楚。

---

[← 上一章：第一个 project build](ch01-03-hello-project-builds.md) · [目录](README.md) · [下一章：变量与显式初始化 →](ch03-01-variables-and-explicit-initialization.md)
