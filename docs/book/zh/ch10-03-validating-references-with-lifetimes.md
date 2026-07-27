# 用生命周期验证引用

上一节说明了：泛型 API 可以描述**一个类型必须支持哪些操作**。但只要 API 接受或返回
引用，就还会多出一个问题：它要怎样描述**这些借用是从哪里来的**？

在今天的 scpp 里，这件事由 `[[scpp::lifetime(...)]]` 标注负责。它们可以让泛型函数和
concept probe 描述多个引用参数之间的关系，也可以让某些 callback 风格 API 表达“这个
引用是新鲜创建出来的，而且只在这次调用期间有效”。

下面每个可运行示例都请保存为 `lifetimes.scpp`，然后这样构建并运行：

```sh
scpp lifetimes.scpp -o lifetimes
./lifetimes
```

## 泛型函数可以指明返回引用来自哪一个输入引用

当一个泛型函数返回引用时，生命周期标注可以说明：这个结果到底绑在哪个输入引用上。

```cpp
import std;

template<typename T>
const T& keep_left(
    const T& left [[scpp::lifetime(a)]],
    const T& right [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return left;
}

int main() {
    int first = 31;
    int second = 8;
    const int& kept = keep_left(first, second);
    int value = kept;
    std::println("{}", value);
    return 0;
}
```

输出：

```text
31
```

这里真正重要的不是具体的 `int`，而是这个关系：返回出来的引用被明确绑定到 `a` 组参
数，而不是 `b` 组参数。

## concept probe 也可以检查多个引用之间的生命周期关系

concept 不只会检查操作名和返回型别。probe 也可以描述：多个引用参数之间必须是什么关
系。

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

class Adder {
public:
    virtual ~Adder() { return; }

public:
    int add(Token& a [[scpp::lifetime(shared)]], Token& b [[scpp::lifetime(shared)]]) const {
        return a.value + b.value;
    }
};

template<typename T>
concept AddsPair = requires(T c, Token& x [[scpp::lifetime(p)]], Token& y [[scpp::lifetime(p)]]) {
    { c.add(x, y) } -> std::same_as<int>;
};

int use_it(const AddsPair auto& adder, Token& left, Token& right) {
    return adder.add(left, right);
}

int main() {
    Adder adder{};
    Token first{};
    first.value = 3;
    Token second{};
    second.value = 4;
    int sum = use_it(adder, first, second);
    std::println("{}", sum);
    return 0;
}
```

输出：

```text
7
```

这个 probe 把 `x` 和 `y` 都放进 `p` 这一组里。`Adder::add()` 里用的是另外一个拼写
`shared`，但这没有关系：重点是，这两个声明里都表达了“这两个参数属于同一个共享组”。

## `[[scpp::lifetime(any)]]` 可以让 callback 接受一次新鲜创建出来的借用

有些泛型 API 会在自己的函数体内部创建一个值，然后把它的引用传给 callback。对这种模
式来说，callback 不可能事先写出那个具体生命周期的名字，因为这个生命周期是由 callee
当场创造出来的。

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

template<typename T>
concept AcceptsToken = requires(T callback, Token& tok [[scpp::lifetime(any)]]) {
    { callback(tok) } -> std::same_as<void>;
};

void with_fresh_token(AcceptsToken auto&& callback) {
    Token tok{};
    tok.value = 42;
    callback(tok);
    return;
}

int main() {
    int result = 0;
    with_fresh_token([&result](Token& tok [[scpp::lifetime(any)]]) {
        result = tok.value;
        return;
    });
    std::println("{}", result);
    return 0;
}
```

输出：

```text
42
```

保留组名 `any` 的意思是“这次调用现场新造出来的一个生命周期”。这让包装函数可以把自
己局部 `Token` 的引用传进去，同时 callback 仍然不能把这个借用当成某个更长寿命的命
名分组来使用。

## 今天 scpp 里关于泛型 API 生命周期的工作规则

到目前为止，实用规则可以先记成这样：

- 当泛型 API 需要描述借用如何跨调用边界关联时，就在引用参数上使用 `[[scpp::lifetime(name)]]`；
- 如果返回引用绑定到某一个特定输入分组，就写出匹配的返回标注；
- concept probe 可以检查生命周期分组关系，不只是检查操作名和返回型别；
- 这些分组名字只在各自声明内部有意义，所以 probe 里的 `p` 不需要复用 callee 自己的
  拼写；
- 对于会把 callee 内部新鲜借用交给 callback 的 API，使用 `[[scpp::lifetime(any)]]`。

这样一来，第 10 章开头需要的工具就齐了：泛型定义、concept 描述的操作要求，以及用于
引用的生命周期标注。下一章就可以在这套基础上继续讲，看看这个项目如何自动验证行为。

---

[← 上一章：用 concept 描述共享要求](ch10-02-defining-shared-requirements-with-concepts.md) · [目录](README.md) · [下一章：编译并运行式测试 →](ch11-01-compile-and-run-tests.md)
