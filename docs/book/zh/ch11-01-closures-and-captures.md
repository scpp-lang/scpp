# 闭包与捕获

在泛型函数、concept 与生命周期之后，下一步很自然就是局部 callback 代码。

在今天的 scpp 里，闭包直接复用普通 C++ lambda 语法。最重要的工作模型其实很简单：闭
包就是一个小型匿名对象，而捕获列表描述的就是这个对象到底在**存什么**，或者在**借什
么**。

下面每个可运行示例都请保存为 `closures.scpp`，然后这样构建并运行：

```sh
scpp closures.scpp -o closures
./closures
```

## 闭包可以传给一个泛型 callback 参数

最常见的用法，就是把一段短小的局部动作交给另一个函数。

```cpp
import std;

template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

void for_each_doubled(std::span<int> s, IntConsumer auto&& f) {
    int i = 0;
    while (i < s.size) {
        f(s[i] * 2);
        i = i + 1;
    }
    return;
}

int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    int sum = 0;
    for_each_doubled(s, [&sum](int x) { sum = sum + x; });
    std::println("{}", sum);
    return 0;
}
```

输出：

```text
12
```

这个闭包捕获了对 `sum` 的可变引用，所以每一次 callback 调用，都会更新同一个外层变
量。

## 混合捕获形式可以让同一个闭包同时借用一些值、拷贝另一些值

捕获列表会明确写出：哪些外部名字会变成按值拥有的字段，哪些会变成按引用借用的字段。

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    int first = apply([&, a](int z) -> int { return a + b + z; }, 3);
    int second = apply([=, &b](int z) -> int { return a + b + z; }, 3);
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

输出：

```text
18
18
```

`[&, a]` 的意思是“其他名字都按引用借用，但 `a` 自己按值拷贝”。`[=, &b]` 刚好相
反：其他名字都按值拷贝，但 `b` 自己按引用借用。

## 在方法里，`this` 必须显式捕获

在方法内部，闭包当然可以再回调到 receiver 上，但 `this` 必须明确写在捕获列表里。

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

class Multiplier {
public:
    virtual ~Multiplier() { return; }

private:
    int factor_{};

public:
    Multiplier(int factor) : factor_{factor} {
        return;
    }

    int scale(int x) const {
        return x * this->factor_;
    }

    int use_closure(int z) {
        return apply([this](int value) -> int { return this->scale(value); }, z);
    }
};

int main() {
    Multiplier m{7};
    int answer = m.use_closure(3);
    std::println("{}", answer);
    return 0;
}
```

输出：

```text
21
```

这个显式的 `[this]` 会把生命周期边界继续保留在表面上。方法里的闭包并不会获得一个绕
过借用规则的隐式后门。

## 今天 scpp 里关于闭包的工作模型

到目前为止，实用规则可以先记成这样：

- 最好把闭包理解成一个小型匿名对象；
- 按值捕获会变成这个对象自己的普通拥有字段；
- 按引用捕获会变成借用字段，所以闭包值会跟其他持有引用的对象一样，遵守同一套生命周
  期推理；
- 把闭包传给另一个函数时，通常走的是一个由 concept 约束的普通泛型参数；
- 在方法内部，`this` 必须显式捕获。

这已经足够写出今天 scpp 里的普通 callback 风格代码了。下一节可以继续留在这个主题
里，再加入泛型 lambda。

---

[← 上一章：用生命周期验证引用](ch10-03-validating-references-with-lifetimes.md) · [目录](README.md)
