# 标量数据类型

上一节重点讲的是“变量该怎样声明”。这一节把视线转向这些变量里面到底装着什么值。

对于刚开始写 scpp 程序的人来说，先掌握四种标量类型就已经够做很多事了：

- `int`：整数；
- `double`：带小数部分的数；
- `bool`：真 / 假判断；
- `char`：单个字符。

和前一节一样，下面这些短小示例都可以保存成 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 整数与小数

`int` 通常是计数、下标以及其它整数计算的起点；而当分数 / 小数部分真的重要时，
`double` 会是更常见的选择。

```cpp
import std;

int main() {
    int left = 10 - 3;
    double price = 1.25 + 0.5;

    std::println("left = {}, price = {}", left, price);
    return 0;
}
```

输出：

```text
left = 7, price = 1.75
```

注意这里的两个计算各自待在自己的类型里：`int` 表达式完全是整数运算，`double`
表达式完全是浮点运算。

如果以后你需要的不是 `int` / `double` 这种“顺手”的写法，而是精确位宽，那么 scpp
也提供了 `int32_t`、`uint64_t`、`float64_t` 之类的名字。

## `bool` 应该来自真正的条件

在 scpp 里，条件本身就应该已经是 `bool`。比较表达式会直接给出这样的 `bool` 值。

```cpp
import std;

int main() {
    int lives = 3;
    bool keep_playing = lives > 0;

    if (keep_playing) {
        std::println("keep playing");
        return 0;
    }

    std::println("game over");
    return 0;
}
```

输出：

```text
keep playing
```

这和普通 C++ 有一个细小但很重要的区别：scpp 不会要求你把任意整数当成“truthy /
falsey”来理解。你应该明确写出像 `lives > 0` 这样的比较，再把结果保存在 `bool`
里。

## `char` 表示单个字符

当一个字节大小的单字符值就足够时，就用 `char`。

```cpp
import std;

int main() {
    char grade = 'A';

    std::println("grade = {}", grade);
    return 0;
}
```

输出：

```text
grade = A
```

这里单引号很重要。`'A'` 是一个 `char`；而 `"A"` 表示的是文本，不是单个字符值。

## 再记住一条规则

scpp 会把这些标量类型明确地区分开来。`bool`、`char`、`int`、`double` 不会静默地
互相转换。所以前面的例子才会让每个表达式都保持在同一种标量类型里，并在需要
`bool` 时写出真正的比较。

一开始这会比普通 C++ 更严格一点，但它也让每次计算到底是什么类型，读起来更一目了
然。

下一节会继续沿着这些基础往前走，把计算和动作打包进具名函数里。

---

[← 上一章：变量与显式初始化](ch03-01-variables-and-explicit-initialization.md) · [目录](README.md) · [下一章：函数 →](ch03-03-functions.md)
