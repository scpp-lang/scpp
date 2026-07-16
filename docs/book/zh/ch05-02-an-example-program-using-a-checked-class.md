# 一个使用受检查 class 的示例程序

上一节里，我们学了如何定义 `struct` 和 `class`。这一节，我们用一个小程序把
`class` 真正用起来，让它的动机更具体一些。

假设我们想处理长方形。一开始，我们也许会把宽度和高度放在分离的变量里，再写接
收这些分离参数的自由函数。

下面每个可运行示例都可以保存成 `checked-class.scpp`，然后这样构建并运行：

```sh
scpp checked-class.scpp -o checked-class
./checked-class
```

## 从分离的值开始

这样写是可行的，但函数签名每次都得重复“这两个值其实是一组”的关系。

```cpp
import std;

int area(int width, int height) {
    return width * height;
}

int main() {
    int width{30};
    int height{50};
    std::println("area = {}", area(width, height));
    return 0;
}
```

输出：

```text
area = 1500
```

这个程序本身没有错。问题在于：`width` 和 `height` 显然属于一起，但类型系统现
在还不知道这件事。

## 把这些数据重构成一个 `class`

当几个相关的值天然属于一起时，把它们放进同一个类型里，会让程序更容易读，也更
不容易意外把它们弄混。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    return 0;
}
```

输出：

```text
frame area = 1500
```

现在，程序可以用一个名字——`Rectangle`——来表示一件东西，而不是到处传三个必须
同步的分离值。

这个例子也说明了为什么在 scpp 里有时 `class` 才是自然选择：`Rectangle` 存着一
个 `std::string` 名字，所以它不能是 `struct`。

## 自由函数可以借用整个对象

一旦数据被收进一个值里，辅助函数就可以接收一个参数，而不是多个分离参数。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

bool can_hold(const Rectangle& outer, const Rectangle& inner) {
    return outer.width > inner.width && outer.height > inner.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    std::println("{} holds {} = {}", frame.name.c_str(), card.name.c_str(), can_hold(frame, card));
    return 0;
}
```

输出：

```text
frame area = 1500
frame holds card = true
```

注意这些签名：

- `area(const Rectangle& rectangle)`
- `can_hold(const Rectangle& outer, const Rectangle& inner)`

这些 `const Rectangle&` 参数就是共享借用，和第 4 章里的引用类型一样。函数可以
读取这个长方形，但不会拿走它的所有权。

## 自由函数也可以通过 `T&` 修改对象

如果一个辅助函数需要更新对象，就可以接收 `Rectangle&`。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

void rename(Rectangle& rectangle, const char* next_name) {
    rectangle.name = std::string{next_name};
    return;
}

int main() {
    Rectangle frame{"draft", 30, 50};
    rename(frame, "published");
    std::println("{}", frame.name.c_str());
    return 0;
}
```

输出：

```text
published
```

所以，一个受检查的 `class` 已经能给我们一个很有用的位置来保存相关数据，而普通
函数则负责描述围绕这些数据的操作。

## 为方法做准备

现在，`area`、`can_hold` 和 `rename` 都还是自由函数。这完全没有问题；有时这就
是你想要的形态。

但它们也都明显是在“描述 `Rectangle` 会做什么”。下一节会把这一类程序再往前推
一步：用方法和 `this` 把这些操作移动到类型自身上。

---

[← 上一章：定义与实例化 `struct` / `class`](ch05-01-defining-and-instantiating-struct-and-class.md) · [目录](README.md)
