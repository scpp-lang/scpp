# 泛型函数与 class

第 10 章从一个最基本的观察开始：有时我们希望**同一种程序形状**能适用于不止
一种具体类型。

在今天的 scpp 里，起点就是熟悉的 C++ template 语法。一个泛型定义会写出一个或多
个类型参数，而每一次具体使用都会用真正的类型去实例化它。

下面每个可运行示例都请保存为 `generics.scpp`，然后这样构建并运行：

```sh
scpp generics.scpp -o generics
./generics
```

## 一个泛型函数可以同时抽象多个不同的具体参数类型

函数模板可以把一种操作写一次，然后让不同调用点各自选择自己的具体类型。

```cpp
import std;

template<typename T, typename U>
T first(T left, U right) {
    return left;
}

int main() {
    std::println("{} {}", first(42, true), first('S', 9));
    return 0;
}
```

输出：

```text
42 S
```

这两个调用并不需要先统一成某一个共同参数类型。第一次调用里，`T = int`、
`U = bool`；第二次调用里，`T = char`、`U = int`。一个泛型函数头就覆盖了这两种具
体情况。

## 一个泛型 class 可以存放实例化时指定的具体类型值

泛型 class 使用同样的 `template<typename ...>` 头部，然后在字段和方法里直接使用
这些类型参数。

```cpp
import std;

template<typename T>
class Holder {
public:
    virtual ~Holder() { return; }

private:
    T item_;

public:
    Holder(const T& item) : item_{item} {
        return;
    }

    T get() const {
        return this->item_;
    }
};

int main() {
    Holder<int> number{42};
    Holder<char> initial{'G'};
    std::println("{} {}", number.get(), initial.get());
    return 0;
}
```

输出：

```text
42 G
```

`Holder<int>` 和 `Holder<char>` 来自同一个 class 定义，但在最终程序里，它们仍然是
两个不同的、已经实例化好的具体类型。

## 每个实例化都可以分别解析依赖类型参数的数据布局

泛型 class 并不只是“一个在运行时抹掉类型信息的通用盒子”。不同实例化可以产生不
同的具体存储布局。

```cpp
import std;

template<typename T>
class Buffer {
public:
    virtual ~Buffer() { return; }

    char storage_[sizeof(T)]{};

    int size() const {
        return static_cast<int>(sizeof(this->storage_));
    }
};

struct OneByte {
    char value;
};

struct EightBytes {
    int left;
    int right;
};

int main() {
    Buffer<int> ints{};
    Buffer<OneByte> one{};
    Buffer<EightBytes> pair{};
    std::println("{} {} {}", ints.size(), one.size(), pair.size());
    return 0;
}
```

输出：

```text
4 1 8
```

所以 `Buffer<int>`、`Buffer<OneByte>` 和 `Buffer<EightBytes>` 并不是都在共用一模一
样的布局。每一个实例化都会针对自己的具体 `T`，分别解析出 `sizeof(T)`。

## 第 10 章开头关于泛型代码的工作模型

到目前为止，实用规则可以先记成这样：

- 用普通的 `template<typename ...>` 头部来写泛型函数和泛型 class；
- 每一次具体使用，都会把这些类型参数替换成真实类型；
- 一个泛型定义可以服务很多不同的具体调用，或很多不同的 class 实例化类型；
- 依赖类型参数的字段和方法体，会针对每个实例化分别解析；
- 下一节会加入 concept，它能让 API 不只表达“这里放某种类型”，还表达“这种类型
  必须支持这些操作”。

这样一来，本章后面部分就有了共同基础：泛型函数家族、泛型 class 家族，以及由它
们各自生成出来的具体实例化类型。

---

[← 上一章：如何在 fail-fast、`std::optional<T>` 与 `std::expected<T, E>` 之间做选择](ch09-06-choosing-between-fail-fast-optional-and-expected.md) · [目录](README.md) · [下一章：用 concept 描述共享要求 →](ch10-02-defining-shared-requirements-with-concepts.md)
