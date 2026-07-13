# 什么是所有权？

所有权是一套规则：它让 scpp 能在没有垃圾回收器、也不用你在日常代码里手写 `delete` 的情况下，自动完成资源清理。

一旦一个值拥有的不再只是最简单的标量，而是堆内存、文件句柄、socket，或者其它必须“恰好清理一次”的资源，这套规则就开始变得重要。

下面每个短小示例都可以保存成 `ownership.scpp`，然后这样构建并运行：

```sh
scpp ownership.scpp -o ownership
./ownership
```

## 栈与堆

像 `int` 这样的标量，会直接放在保存它的那个局部变量里。`std::string` 这样的值则不同：局部对象本身仍然很小，但它管理的文本放在别处。

`std::string` 会管理存放在堆上的文本数据，因此它能保存长度在编译期不必确定、并且可以在运行时增长的文本。

这正是所有权存在的主要原因：总得有谁来决定那块堆内存该在什么时候释放。

可以先把这套模型记成三条实用规则：

- 每一份被拥有的资源，都由某一个仍然活着的拥有对象负责；
- `std::move(x)` 会把这份责任从 `x` 转移出去，并立刻让 `x` 进入 moved-out 状态；
- 当前拥有者离开作用域时，清理会自动发生。

## 作用域决定清理何时发生

最基础的所有权概念，就是作用域。一个局部对象从声明处开始有效，一直到包住它的那个代码块结束。

```cpp
import std;

class Note {
private:
    const char* name{};

public:
    Note(const char* text) : name{text} {
        std::println("start {}", this->name);
        return;
    }

    ~Note() {
        std::println("drop {}", this->name);
        return;
    }
};

int main() {
    std::println("before inner");
    {
        Note inner{"inner"};
        std::println("inside inner");
    }
    std::println("after inner");
    return 0;
}
```

输出：

```text
before inner
start inner
inside inner
drop inner
after inner
```

`inner` 在执行走到它的声明处时创建，而它的析构函数会在内层代码块结束时自动运行。这就是 scpp 里最普通的 RAII 故事：作用域决定清理何时发生。

## `std::string` 拥有堆上的数据

`std::string` 很适合拿来当第一个所有权示例，因为它的大小可以在运行时变化。

```cpp
import std;

int main() {
    std::string title{"scpp"};
    title.append(" book");

    std::println("{} ({} bytes)", title.c_str(), title.length());
    return 0;
}
```

输出：

```text
scpp book (9 bytes)
```

局部变量 `title` 拥有这个 `std::string` 值。由于这个字符串会管理堆上分配的文本数据，所以当 `title` 离开作用域时，它的析构函数就会释放那块内存。

## move 会转移所有权

当一个拥有型值应该改由别人负责时，就用 `std::move`。

```cpp
import std;

int main() {
    std::string first{"owner"};
    std::string second{std::move(first)};
    second.append("ship");

    std::println("{}", second.c_str());
    return 0;
}
```

输出：

```text
ownership
```

在 scpp 里，`std::move(first)` 不只是一个库辅助函数。语言本身把这种语法当成“立刻把 `first` 置为 moved-out”的操作。到了这一步之后，`first` 就不能再用了，而 `second` 成了那个字符串对象唯一还活着的拥有者。

这就是 scpp 避免双重析构的方式：一旦所有权已经移走，旧拥有者既不会再被使用，也不会在作用域结束时按“已初始化对象”再次析构。

## 复制和 move 是两回事

有些值会被复制，而不是被 move。像 `int`、`bool`、`char`、`double` 这样的普通标量就是这样。

```cpp
import std;

int main() {
    int x{5};
    int y{x};
    y = y + 1;

    std::println("x = {}", x);
    std::println("y = {}", y);
    return 0;
}
```

输出：

```text
x = 5
y = 6
```

修改 `y` 不会影响 `x`，因为这里发生的是值复制。

对于 class 类型，复制并不是自动存在的。一个 class 只有真的定义了复制行为，才是可复制的。`std::string` 现在已经支持深拷贝，所以普通的复制构造和复制赋值都会得到一个新的拥有型字符串值。

像 `std::string second{first};` 这样的花括号初始化会调用 copy constructor，而 `third = first;` 会调用 copy assignment：

```cpp
import std;

int main() {
    std::string first{"book"};
    std::string second{first};
    std::string third{"draft"};
    third = first;
    second.append(" chapter");
    third.append(" notes");

    std::println("first = {}", first.c_str());
    std::println("second = {}", second.c_str());
    std::println("third = {}", third.c_str());
    return 0;
}
```

输出：

```text
first = book
second = book chapter
third = book notes
```

`second` 和 `third` 都各自拥有自己的字符串值。修改任意一个副本，都不会影响 `first`。

## 所有权与函数

按值传参和按值返回，遵循的是同一套所有权规则。

class 值可以通过按值返回，把所有权交回给 caller。函数写成 `return std::move(word);` 时，所有权会从这个局部值转移到返回值上：

```cpp
import std;

std::string make_word() {
    std::string word{"hello"};
    return std::move(word);
}

int main() {
    std::string local{make_word()};
    std::println("{}", local.c_str());
    return 0;
}
```

输出：

```text
hello
```

`make_word()` 返回的是一个拥有型 `std::string`，而 caller 里的 `local` 会成为新的拥有者。

如果一个 class 值通过 `std::move` 按值传进函数，那么 callee 也会接管这个实参的所有权：

```cpp
import std;

void print_word(std::string text) {
    std::println("{}", text.c_str());
    return;
}

int main() {
    std::string word{"hello"};
    print_word(std::move(word));
    return 0;
}
```

输出：

```text
hello
```

在 `std::move(word)` 之后，形参 `text` 就成了 `print_word` 里面那个仍然活着的拥有者。

如果某个 class 类型**有**复制行为，那么把一个左值按值传进函数时，就可以通过复制创建出新的拥有对象：


```cpp
import std;

class Label {
private:
    const char* text{};

public:
    Label(const char* value) : text{value} {
        return;
    }

    Label(const Label& other) : text{other.text} {
        std::println("copy {}", this->text);
        return;
    }

    const char* c_str() const {
        return this->text;
    }
};

Label echo_label(Label label) {
    return label;
}

int main() {
    Label first{"ticket"};
    Label second{echo_label(first)};
    std::println("{}", second.c_str());
    return 0;
}
```

输出：

```text
copy ticket
ticket
```

这次运行只打印了一次 `copy ticket`：把 `first` 按值传进去时，会先把它复制到形参对象里。之后返回这份局部值时，语言仍然可以选择 move 它或者直接复用它；但核心点不变：只要类型可复制，函数边界就能在程序显式进行复制时创建出新的拥有者。

到这里，第一版所有权模型就够用了：

- 作用域结束，会结束拥有者的生命周期；
- `std::move` 会转移所有权，并让旧拥有者失效；
- 普通标量会廉价复制，而 class 类型只有真的定义了复制行为时才可复制；
- 函数边界要么 move 所有权，要么按类型定义复制出一个新的拥有者。

下一节会保持这套所有权规则不变，但开始回答一个新问题：如果代码只想临时使用某个值，而不想拿走它的所有权，该怎么办？

---

[← 上一章：控制流](ch03-05-control-flow.md) · [目录](README.md)
