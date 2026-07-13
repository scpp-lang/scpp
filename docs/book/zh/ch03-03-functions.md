# 函数

变量给值起名字；函数则给“可以重复做的工作”起名字。

这正好是数据类型之后最自然的一步：当你已经知道程序里有哪些值之后，就可以开始把
计算打包成可重复调用的片段。

下面每个短小示例都可以保存成 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## 定义并调用一个函数

一个函数定义有四个很熟悉的部分：

- 返回类型；
- 函数名；
- 放在圆括号里的形参列表；
- 放在花括号里的函数体。

```cpp
import std;

int double_value(int value) {
    return value * 2;
}

int main() {
    int doubled = double_value(21);

    std::println("doubled = {}", doubled);
    return 0;
}
```

输出：

```text
doubled = 42
```

`double_value(21)` 这次调用会把 `21` 交给形参，然后从函数里拿回一个 `int` 结果。

## 形参本身也是局部名字

在函数体内部，每个形参都可以看成一个带有初始值的局部变量。

```cpp
import std;

int add_one(int value) {
    value = value + 1;
    return value;
}

int main() {
    int score = 10;
    int next = add_one(score);

    std::println("score = {}, next = {}", score, next);
    return 0;
}
```

输出：

```text
score = 10, next = 11
```

`add_one` 里面改动 `value`，并不会反过来改写 `main` 里的 `score`。函数处理的是它
自己的形参变量，然后把结果返回出来。

## 函数也可以返回 `bool`

函数不一定非要返回数字。它也可以返回一个 `bool`，让程序的其它部分直接拿来判断。

```cpp
import std;

bool can_level_up(int score, int bonus) {
    return score + bonus >= 100;
}

int main() {
    bool ready = can_level_up(80, 25);

    std::println("ready = {}", ready);
    return 0;
}
```

输出：

```text
ready = true
```

当一个函数要回答的是“是 / 否问题”，而不是去计算一个新数字或一段文本时，这种写法
就很合适。

## 再记住一条重要的类型规则

函数的形参类型和返回类型都是显式写出来的，而 scpp 会把不同标量类型明确区分开来。
这意味着：函数调用应该传入它真正要求的类型，而不是依赖隐藏的隐式转换。

实际写代码时，这反而会让函数调用更容易读：看一眼函数签名，就知道那里到底应该放
什么类型的值。

下一节会继续保持“小而实用”的风格，但主题会换成注释：怎样给人类读者解释代码，同
时又不改变编译器真正看到的程序。

---

[← 上一章：标量数据类型](ch03-02-scalar-data-types.md) · [目录](README.md) · [下一章：注释 →](ch03-04-comments.md)
