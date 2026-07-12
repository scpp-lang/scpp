# 变量与显式初始化

在猜数字小游戏那一章里，我们已经用局部变量记住了秘密数字和玩家最新一次的猜测。
现在可以把节奏放慢一点，单独看看这些声明本身到底意味着什么。

这一节里的短小示例都可以保存成 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 局部变量默认就是可变的

一个普通的局部变量会给某个值起一个名字，而这个值稍后仍然可以在同一作用域里改变。

```cpp
import std;

int main() {
    int counter = 0;
    counter = counter + 1;
    counter = counter + 1;

    std::println("counter = {}", counter);
    return 0;
}
```

输出：

```text
counter = 2
```

这里真正重要的不只是 `counter` 的值变了，还在于：它在整个生命周期里始终保持同一
个类型。它一旦被声明成 `int`，之后就一直是 `int`。

## `const` 会让局部变量变成只读

如果某个局部变量只应该在声明时赋值一次、之后保持不变，就写上 `const`。

```cpp
import std;

int main() {
    const int target = 21;
    int doubled = target + target;

    std::println("doubled = {}", doubled);
    return 0;
}
```

输出：

```text
doubled = 42
```

`target` 会在声明时完成初始化，之后就不能再次赋值了。只要一个名字代表的是你不希
望被意外改掉的值，这种写法就很有用。

## 每个局部变量都必须带初始化器

现在的 scpp 不允许写出 `int score;` 这种“裸声明”局部变量。每个局部变量在声明时
都必须把自己的起始值写清楚。

```cpp
import std;

int main() {
    int score{};
    int level{3};
    int bonus = 7;
    bool finished{};

    std::println("score = {}, level = {}, bonus = {}, finished = {}", score, level, bonus, finished);
    return 0;
}
```

输出：

```text
score = 0, level = 3, bonus = 7, finished = false
```

这个小例子顺手展示了最常见的三种写法：

- `int score{};` 用空花括号，表示“拿这个类型的零值 / 默认值”；
- `int level{3};` 用带实参的花括号，显式给出起始数据；
- `int bonus = 7;` 用 `=` 接一个表达式来初始化。

真正重要的规则是：初始化器不是可选项。哪怕你想要的只是零值或者 `false`，scpp
也要求你在声明处明确写出来。

## 一个简单的日常习惯

在日常代码里，先抓住这三条就够了：

- 需要变化的值，用普通局部变量；
- 初始化后就不该再变的值，用 `const`；
- 每个局部变量都在声明时立刻初始化，不要先声明、后补值；
- 想要零值 / 默认值时用 `{}`，已经知道起始数据时用 `= value` 或 `{value}`。

下一节会继续保持这种“小程序、慢拆解”的节奏，但重点会更直接地放到这些变量所承
载的数据类型本身。

---

[← 上一章：做一个猜数字小游戏](ch02-00-guessing-game.md) · [目录](README.md) · [下一章：标量数据类型 →](ch03-02-scalar-data-types.md)
