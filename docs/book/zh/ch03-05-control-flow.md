# 控制流

变量负责保存值，函数负责把一段工作打包成可复用的部件，而控制流决定接下来运行哪一段
工作，以及它要运行多少次。

在当前的 scpp 里，学习者现在真正能用上的主要控制流工具包括 `if`、`while`、经典
`for`，以及基于范围的 `for`。这一节会用一些你今天就能写、也能跑起来的小程序，依次
介绍它们。

下面每个短小示例都可以保存成 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## `if` 只会在条件为真时运行代码

当一段代码只应该在某个条件成立时执行，就用 `if`。

```cpp
import std;

int main() {
    int temperature = 33;

    if (temperature > 30) {
        std::println("It is warm outside.");
    }

    return 0;
}
```

输出：

```text
It is warm outside.
```

`if (...)` 里的条件本身就应该已经是一个 `bool`。这里的 `temperature > 30` 是一个比
较表达式，因此它产生的正是 `if` 想要的那种值。

## `else if` 与 `else` 用来在不同路径之间做选择

当程序有不止一条可能路径时，可以用 `else if` 把条件串起来，再用 `else` 处理兜底情
况。

```cpp
import std;

int main() {
    int score = 85;

    if (score < 60) {
        std::println("try again");
    } else if (score < 90) {
        std::println("you passed");
    } else {
        std::println("excellent");
    }

    return 0;
}
```

输出：

```text
you passed
```

这些分支会从上到下依次检查。一旦有某个条件为真，对应分支就会执行，后面的分支也就
不会再看了。

## `while` 会在条件保持为真时重复执行工作

当同一段代码应该一直重复，直到某个条件不再成立时，就用 `while`。

```cpp
import std;

int main() {
    int count = 3;

    while (count > 0) {
        std::println("{}!", count);
        count = count - 1;
    }

    std::println("Go!");
    return 0;
}
```

输出：

```text
3!
2!
1!
Go!
```

一个 `while` 循环既需要条件，也需要会变化的状态。如果 `count` 从来不变，这个循环就
永远不会结束。

## 经典 `for` 会把循环的设置集中写在一处

经典 `for` 会把三个部分放进同一个循环头里：

- 只执行一次的初始步骤；
- 每轮开始前检查的条件；
- 每轮完成后执行的更新步骤。

```cpp
import std;

int main() {
    int total = 0;

    for (int i = 1; i <= 5; i = i + 1) {
        total = total + i;
    }

    std::println("total = {}", total);
    return 0;
}
```

输出：

```text
total = 15
```

这和你用 `while` 写计数循环时表达的是同一个思路，但 `for` 会把循环变量、停止条件和
每轮更新放在一起，更容易一眼看清。

## 基于范围的 `for` 会按顺序访问每个元素

如果你想把数组里的每个元素都访问一遍，基于范围的 `for` 往往是最清楚的写法。

```cpp
import std;

int main() {
    int scores[3]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    int total = 0;

    for (int score : scores) {
        total = total + score;
    }

    std::println("total = {}", total);
    return 0;
}
```

输出：

```text
total = 60
```

这里的循环变量 `score` 会依次用数组里的每个元素来初始化。因为它是按值声明的，所以
即使你修改 `score` 本身，也不会改动底层数组。

## 基于范围的 `for` 也可以配合 `std::span`

基于范围的 `for` 不只适用于定长数组。它也可以配合 `std::span` 使用；`std::span`
是 scpp 里用来借用一段元素序列的视图类型。

```cpp
import std;

int main() {
    int values[3]{};
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    std::span<int> view = values;

    for (auto& value : view) {
        value = value * 2;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

输出：

```text
2
4
6
```

因为循环变量写成了 `auto&`，所以每次迭代里的 `value` 都是在引用 `span` 背后的那个
真实元素。你更新 `value`，原来的数组也会一起被更新。

## 一条实用规则

在决定该用哪种控制流工具时，可以先记住这几条：

- 想在不同路径之间做选择，就用 `if`；
- 想重复一段工作，就用 `while`；
- 如果一个循环天然由“初始化 + 条件 + 更新”组成，就用经典 `for`；
- 如果你想按顺序访问数组或 `std::span` 的每个元素，就用基于范围的 `for`；
- 把条件写成明确的比较，让它本身直接产生 `bool`；
- 如果循环最终应该停下来，就要更新那个被条件读取的状态。

这已经足够帮助你理解前面猜数字章节里见过的所有示例，也足够写出很多会分支、会重复、
会计数、也会逐个处理序列元素的小型命令行程序。

---

[← 上一章：注释](ch03-04-comments.md) · [目录](README.md) · [下一章：什么是所有权？→](ch04-01-what-is-ownership.md)
