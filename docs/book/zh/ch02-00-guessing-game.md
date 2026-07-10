# 做一个猜数字小游戏

上一章里，你已经证明了工具链是能工作的。现在该写一个稍微更像样一点的程序了。

这一章我们来做一个很小的猜数字游戏。程序内部保存一个秘密数字，玩家不断输入猜
测，程序则回答：

- 如果猜得比秘密数字小，就输出 “Too small!”
- 如果猜得比秘密数字大，就输出 “Too big!”
- 如果猜对了，就输出 “You win!”

这一章会先用起来、后面再细讲几个 scpp 里的基础概念：

- 可变的局部变量，
- `while` 循环，
- `if` 条件分支，
- 通过 `extern "C"` 调用 C 函数，
- 以及用 `[[scpp::unsafe]]` 明确标出这些调用边界。

现在我们先把秘密数字固定成 `42`。这样可以让程序保持足够小，把注意力先放在语言
最核心的执行流程上。

## 完整程序

创建一个名为 `guessing_game.scpp` 的文件：

```cpp
extern "C" int puts(const char* s);
extern "C" int scanf(const char* fmt, ...);

int main() {
    int secret = 42;
    int guess = 0;

    [[scpp::unsafe]] {
        puts("Guess the number!");
    }
    while (guess != secret) {
        [[scpp::unsafe]] {
            puts("Please input your guess:");
            scanf("%d", &guess);
        }
        if (guess < secret) {
            [[scpp::unsafe]] {
                puts("Too small!");
            }
        } else {
            if (guess > secret) {
                [[scpp::unsafe]] {
                    puts("Too big!");
                }
            } else {
                [[scpp::unsafe]] {
                    puts("You win!");
                }
            }
        }
    }
    return 0;
}
```

编译并运行它：

```sh
scpp guessing_game.scpp -o guessing_game
printf '25\n50\n42\n' | ./guessing_game
```

输出：

```text
Guess the number!
Please input your guess:
Too small!
Please input your guess:
Too big!
Please input your guess:
You win!
```

如果你不是用管道把输入一次性喂给程序，而是直接交互运行它，那么每次输入一个数字、
回车一次就可以。

## 这一章其实已经引入了什么

这个小程序里其实已经塞进了不少东西。

`int secret = 42;` 和 `int guess = 0;` 引入了局部变量。`guess` 的值会随着程序运
行不断变化，所以循环才能一轮一轮地继续猜下去，直到玩家猜中为止。

`while (guess != secret)` 让程序在条件仍为真时持续运行。循环体里，程序会读入一
个新猜测，再把它和秘密数字比较。

`if` / `else` 这条分支链决定了当前应该打印哪一条消息。这也是我们第一次让程序根
据用户输入的数据走向不同的行为分支。

`puts` 和 `scanf` 都来自 C 运行时，不是 scpp 编译器自己能验证的代码，所以每次调
用都放在显式的 `[[scpp::unsafe]]` 块里。这并不意味着“整个程序都不安全”；它的意
思是：你信任外部代码的那个具体边界，被明确而局部地写了出来。

## 为什么先这样写？

如果你熟悉 Rust，可能会注意到：这个版本还没有使用安全的字符串输入 API，也没有
使用随机数库。这是刻意为之——这一章先停留在 scpp 今天已经能清晰支持的能力范围
内。

重要的是：你现在已经写出了一个真实的交互式程序。它会读入输入、保存状态、重复执
行工作，并根据条件选择不同分支。下一章我们会放慢节奏，把这些基础构件逐个拆开来
讲清楚。

---

[← 上一章：第一个 project build](ch01-03-hello-project-builds.md) · [目录](README.md) · [下一章：常见编程概念 →](README.md)
