# 用 concept 描述共享要求

上一节说明了：一个泛型定义可以被很多具体类型实例化。接下来的问题是：API 要怎样表
达**自己期待哪一类类型**？

在今天的 scpp 里，这件事由 `concept` 负责。concept 会给一组操作起一个可复用的名字，
而泛型 API 可以要求调用方提供真正支持这些操作的类型。

下面每个可运行示例都请保存为 `concepts.scpp`，然后这样构建并运行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## concept 会给结构性的要求命名

一个类型之所以满足某个 concept，是因为它真的拥有那些被要求的操作。这里没有单独的
“把这个类型登记成实现了某个 concept”声明。

```cpp
import std;

class Circle {
public:
    virtual ~Circle() { return; }

    int area() const {
        return 314;
    }
};

class Square {
public:
    virtual ~Square() { return; }

    int area() const {
        return 100;
    }
};

template<typename T>
concept Shape = requires(const T& t) {
    { t.area() } -> std::same_as<int>;
};

int area_value(const Shape auto& shape) {
    return shape.area();
}

int main() {
    Circle circle{};
    Square square{};
    int circle_area = area_value(circle);
    int square_area = area_value(square);
    std::println("{}", circle_area);
    std::println("{}", square_area);
    return 0;
}
```

输出：

```text
314
100
```

这个 concept 精确写出了函数体真正需要的东西：给定 `const T&` 之后，调用 `area()`
必须合法，而且结果必须是 `int`。`Circle` 和 `Square` 都在结构上满足这个要求，所以
这两个调用都能通过编译。

## 当同一个受约束类型要出现多次时，完整模板头形式更合适

缩写形式 `Concept auto` 很适合只有一个参数的时候；如果同一个类型参数要出现在多个位
置，完整模板头会更清楚。

```cpp
import std;

class Meter {
public:
    virtual ~Meter() { return; }

private:
    int value_{};

public:
    Meter(int value) : value_{value} {
        return;
    }

    int get() const {
        return this->value_;
    }
};

template<typename T>
concept HasGet = requires(const T& t) {
    { t.get() } -> std::same_as<int>;
};

template<HasGet T>
int sum_pair(const T& left, const T& right) {
    return left.get() + right.get();
}

int main() {
    Meter first{19};
    Meter second{23};
    int total = sum_pair(first, second);
    std::println("{}", total);
    return 0;
}
```

输出：

```text
42
```

这里 concept 仍然是在描述共享要求，但这个函数需要一个被命名出来的 `T`，因为两个
参数必须拥有同一个具体类型。

## 泛型 class 可以让部分操作保持不受约束，再用 `requires` 打开额外能力

有时候，一个泛型 class 应该接受很多类型，但只有某一个方法需要额外能力。这时就把约
束写在那个方法本身上。

```cpp
import std;

template<typename T>
concept Describable = requires(const T& t) {
    { t.magnitude() } -> std::same_as<int>;
};

class Circle {
public:
    virtual ~Circle() { return; }

private:
    int radius_{};

public:
    Circle(int radius) : radius_{radius} {
        return;
    }

    Circle(const Circle& other) : radius_{other.radius_} {
        return;
    }

    int magnitude() const {
        return this->radius_;
    }
};

template<typename T>
class Box {
public:
    virtual ~Box() { return; }

private:
    T item_;

public:
    Box(const T& item) : item_{item} {
        return;
    }

    const T& get() const {
        return this->item_;
    }

    int describe() const requires Describable<T> {
        return this->item_.magnitude();
    }
};

int main() {
    Box<int> number{5};
    Box<Circle> circle{Circle{9}};
    int number_value = number.get();
    int circle_value = circle.describe();
    std::println("{}", number_value);
    std::println("{}", circle_value);
    return 0;
}
```

输出：

```text
5
9
```

`Box<T>` 本身仍然保持广泛可用。`get()` 对任何 `T` 都成立；而 `describe()` 只会在那
个实例化出来的 `T` 满足 `Describable` 时才可用。

## 今天 scpp 里关于 concept 的工作规则

到目前为止，实用规则可以先记成这样：

- 用 `template<typename T> concept Name = requires(...) { ... };` 来定义 concept；
- 一个类型是通过支持那些被要求的操作，在结构上满足这个 concept；
- 只有一个受约束参数时，用 `Concept auto`；
- 如果同一个受约束类型参数要出现在多个位置，就改用完整模板头形式；
- 如果只有泛型 class 的某个方法需要额外能力，就把 `requires Concept<T>` 写在那个方
  法上。

这样一来，第 10 章就多了一块新的词汇：泛型 API 不再只会表达“这里放某种类型”，还
能表达“这里放某种**支持这些操作**的类型”。下一节会再加入生命周期，让这些共享要求
也能安全地谈论引用。

---

[← 上一章：泛型函数与 class](ch10-01-generic-functions-and-classes.md) · [目录](README.md) · [下一章：用生命周期验证引用 →](ch10-03-validating-references-with-lifetimes.md)
