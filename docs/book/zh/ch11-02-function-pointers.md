# 函数指针

上一节讲的是闭包：它们是小型的局部可调用对象。另一个同样常见、而且更老也更直接的可
调用形式，就是**指向具名函数的指针**。

在今天的 scpp 里，函数指针直接复用普通 C 和 C++ 语法。这意味着你可以把它们传来传
去、放进对象里保存，也可以靠目标指针类型来选中某一个重载。

下面每个可运行示例都请保存为 `function-pointers.scpp`，然后这样构建并运行：

```sh
scpp function-pointers.scpp -o function-pointers
./function-pointers
```

## 一个函数名可以初始化匹配的函数指针

普通函数名会衰变成一个匹配类型的函数指针值。

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

int main() {
    int (*fp)(int, int) = add;
    int value = fp(2, 3);
    std::println("{}", value);
    return 0;
}
```

输出：

```text
5
```

这里最重要的观念是：`fp` 只是一个值，它保存的是“等会儿该调用哪一个函数”。通过它调
用时，用的仍然是普通函数调用语法。

## 函数指针可以存进 class 字段里

因为函数指针本来就是普通值，所以 class 完全可以把它保存下来，稍后再使用。

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

class Holder {
public:
    virtual ~Holder() { return; }

private:
    int (*fp_)(int, int) = nullptr;

public:
    Holder(int (*fp)(int, int)) : fp_{fp} {
        return;
    }

    int run(int x, int y) const {
        return this->fp_(x, y);
    }
};

int main() {
    Holder h{&add};
    int value = h.run(4, 5);
    std::println("{}", value);
    return 0;
}
```

输出：

```text
9
```

这里的 `&add` 和上面直接写 `add` 是等价的。有些代码在继续往下传递函数时，会更喜欢
这种显式取地址的写法。

## 目标指针类型可以选中某一个重载

如果有多个函数共用同一个名字，那么目标指针类型会决定你指的到底是哪一个。

```cpp
import std;

int encode(int value) {
    return value + 100;
}

char encode(char value) {
    return value;
}

int main() {
    int (*pick_int)(int) = &encode;
    char (*pick_char)(char) = &encode;
    int first = pick_int(23);
    char second = pick_char('Q');
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

输出：

```text
123
Q
```

所以 `&encode` 自己并不会一直保持含糊不清。只要接收它的目标类型已经明确，重载解析
就知道该选中哪一个具体函数。

## 今天 scpp 里关于函数指针的工作模型

到目前为止，实用规则可以先记成这样：

- 函数指针使用普通 C/C++ 的“指向函数的指针”语法；
- 匹配的函数名，或 `&function_name`，都可以用来初始化它；
- 通过函数指针调用时，用的仍然是普通调用语法；
- 函数指针是普通值，所以可以存进对象里，也可以在 API 之间传递；
- 如果名字本身有重载，目标指针类型会选中匹配的那个重载。

这样一来，第 11 章就有了继闭包之后的第二种可调用构件。下一节可以再加入拥有型包装器，
比如 `std::function` 和 `std::move_only_function`。

---

[← 上一章：闭包与捕获](ch11-01-closures-and-captures.md) · [目录](README.md)
