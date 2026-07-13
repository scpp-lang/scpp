# 注释

注释是写给人看的。它可以用来解释意图、标出一个容易忽略的细节，或者在代码旁边留
下一句短说明，而不会改变编译器真正执行的程序。

所以注释这个主题虽然不大，却很重要：好的注释，往往决定了一段代码只是“能跑”，还
是“过段时间回来再看也容易理解”。

下面每个短小示例都可以保存成 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 行注释

最常见的形式，是以 `//` 开头的行注释。

```cpp
import std;

int main() {
    // This comment is for people, not for the compiler.
    int answer = 40 + 2;

    std::println("answer = {}", answer);
    return 0;
}
```

输出：

```text
answer = 42
```

在那一行里，`//` 后面的内容都会被编译器忽略。

## 块注释

如果一行短说明不够，就可以用块注释。

```cpp
import std;

int main() {
    /* A block comment can cover more than one line
       when one short note is not enough. */
    int total = 20 + 22;

    std::println("total = {}", total);
    return 0;
}
```

输出：

```text
total = 42
```

块注释适合稍微长一点、而且确实应该紧挨着那段代码出现的说明。

## 写在语句旁边的短注释

有时候，最清楚的说明就是直接跟在那条语句后面的一个小提醒。

```cpp
import std;

int main() {
    int score = 7; // starting score for this round
    score = score + 5;

    std::println("score = {}", score);
    return 0;
}
```

输出：

```text
score = 12
```

这种写法最适合非常简短的说明。如果解释开始变长，就最好把注释放到语句上方去。

## 一条实用规则

好的注释更应该解释“为什么”，而不只是重复“做了什么”。如果代码本身已经非常清楚
地表达了它正在做什么，再用文字重复一遍，往往只会增加噪音。更值得写注释的场景是：

- 代码本身看不出来的意图；
- 读者应该知道的前提假设；
- 在更长的函数或模块里，帮助快速定位的说明。

下一节会重新回到程序行为和控制流：怎样用 `if` 在不同路径之间做选择，以及怎样用
`while` 重复一段工作。

---

[← 上一章：函数](ch03-03-functions.md) · [目录](README.md)
