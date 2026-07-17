# 方法与 `this`

上一节里，我们把相关数据收进了一个受检查的 `class`，并且用了像
`area(rectangle)`、`can_hold(outer, inner)` 这样的自由函数。

这些函数当然能工作，但它们显然也都是“关于同一个类型”的操作。方法让我们把这些
行为直接移动到类型本身上。

下面每个可运行示例都可以保存成 `methods.scpp`，然后这样构建并运行：

```sh
scpp methods.scpp -o methods
./methods
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输
出，请把文件保存成诊断块里显示的那个描述性文件名。

## 定义方法

方法就是写在 `struct` 或 `class` 定义里的函数。接收者对象，就是点号前面的那个
值。

```cpp
import std;

class Rectangle {
private:
    std::string name;
    int width{};
    int height{};

public:
    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }

    bool can_hold(const Rectangle& other) const {
        return this->width > other.width && this->height > other.height;
    }

    const char* label() const {
        return this->name.c_str();
    }
};

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.label(), frame.area());
    std::println("{} holds {} = {}", frame.label(), card.label(), frame.can_hold(card));
    return 0;
}
```

输出：

```text
frame area = 1500
frame holds card = true
```

现在，我们不再写 `area(frame)`，而是写 `frame.area()`。这种写法更清楚地表明：
这个操作属于 `Rectangle`。

## `this` 指向接收者

在方法内部，`this` 的意思就是“调用这个方法的那个对象”。你经常会写
`this->field` 来把这一点说清楚，尤其是在参数名会和字段名冲突的时候。

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    void resize(int width, int height) {
        this->width = width;
        this->height = height;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Rectangle rect{2, 3};
    rect.resize(4, 5);
    std::println("{}", rect.area());
    return 0;
}
```

输出：

```text
20
```

这里，参数也叫 `width` 和 `height`，所以 `this->width` 的意思就是“接收者上的字
段”，而不是局部参数。

## `const` 方法只读取接收者

如果一个方法只需要观察对象，就把它标成 `const`。这样一来，这个方法就可以通过
共享借用（比如 `const Rectangle&`）来调用。

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int describe(const Rectangle& rectangle) {
    return rectangle.area();
}

int main() {
    Rectangle rect{6, 7};
    std::println("{}", describe(rect));
    return 0;
}
```

输出：

```text
42
```

这和第 4 章里的借用模型是同一个故事。`const` 方法面对的是接收者的共享、只读视
图。

## 非 `const` 方法需要可变接收者

如果一个方法可能更新对象，就不要写 `const`。这样一来，它就不能通过 `const`
引用来调用。

```cpp
class Counter {
private:
    int value{};

public:
    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

void tick(const Counter& counter) {
    counter.increment();
    return;
}

int main() {
    Counter counter{5};
    tick(counter);
    return 0;
}
```

编译器输出：

```text
nonconst_method_on_const_ref_fail.scpp:21:5: error: cannot call non-const member function 'increment' through a read-only (const) receiver
 21 |     counter.increment();
    |     ^
```

所以，方法上的 `const` 并不是装饰。它会改变接收者是如何被借用的，也会改变哪些
调用是允许的。

## 调用方法会借用接收者

scpp 检查方法调用的方式，和检查其它借用是一样的。一个会修改对象的方法，需要拿
到整个接收者对象的可变访问权。

```cpp
class Counter {
public:
    int value{};

    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

int main() {
    Counter counter{5};
    int& value_ref = counter.value;
    counter.increment();
    return value_ref;
}
```

编译器输出：

```text
public_field_borrow_conflict.scpp:22:5: error: cannot use 'counter' while it is mutably borrowed
 22 |     counter.increment();
    |     ^
```

对 `counter.value` 的借用一直活到最后那个 `return`，所以 `counter.increment()`
就不能同时再拿到同一个接收者的可变访问权。

下一节会继续使用方法，但会把重点放到 `[[scpp::unsafe]]` 这样的所有权边界上。

---

[← 上一章：一个使用受检查 class 的示例程序](ch05-02-an-example-program-using-a-checked-class.md) · [目录](README.md)
