# `std::span` 与其它非拥有视图

所有权解释的是谁负责清理资源。引用解释的是：代码怎样才能在不接走这份责任的前提下，暂时使用一个值。`std::span<T>` 则把同一套“借用”思路应用到一整段连续元素上。

在今天的 scpp 里，`std::span` 是最主要的标准非拥有视图类型。你可以把一个 span 理解成一个很小的视图值：它把指向首元素的指针和长度配在一起。span 本身并不拥有这些元素；真正的拥有者仍然是那个数组。

下面每个可运行示例都可以保存成 `span.scpp`，然后这样构建并运行：

```sh
scpp span.scpp -o span
./span
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输出，请把文件保存成诊断块里显示的那个描述性文件名。

## 从定长数组构造 span

今天，正常的构造路径就是从定长数组开始。

```cpp
import std;

int main() {
    int numbers[4]{};
    numbers[0] = 7;
    numbers[1] = 8;
    numbers[2] = 9;
    numbers[3] = 10;
    std::span<int> view = numbers;
    int length = view.size;
    std::println("{}", length);
    std::println("{}", view[2]);
    return 0;
}
```

输出：

```text
4
9
```

`view` 借用的是这个数组。构造这个 span 时，四个元素并没有被复制，所有权也没有从 `numbers` 身上移走。

## 把 span 传给函数做只读访问，而不复制元素

如果一个函数只需要读取一段序列，就可以接收 `std::span<const T>`。

```cpp
import std;

int sum(std::span<const int> values) {
    int total = 0;
    for (int value : values) {
        total = total + value;
    }
    return total;
}

int main() {
    int numbers[4]{};
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    std::println("sum = {}", sum(numbers));
    return 0;
}
```

输出：

```text
sum = 100
```

`sum(numbers)` 这个调用会在调用点构造出 span 视图。把 span 按值传进去时，被复制的只是这个很小的视图对象，而不是底层数组里的元素。

## 可变 span 可以更新调用者的数组

如果一个函数需要原地修改已有元素，就接收 `std::span<T>`。

```cpp
import std;

void double_all(std::span<int> values) {
    for (auto& value : values) {
        value = value * 2;
    }
    return;
}

int main() {
    int numbers[3]{};
    numbers[0] = 3;
    numbers[1] = 4;
    numbers[2] = 5;
    double_all(numbers);
    for (int value : numbers) {
        std::println("{}", value);
    }
    return 0;
}
```

输出：

```text
6
8
10
```

`double_all` 依然不会拥有这个数组。它拿到的是一个可变的非拥有视图，经由这个视图完成写入，而整个过程中拥有者始终还是调用者。

## `std::span<const T>` 是只读的

一旦元素类型写成 `const`，得到的就是共享、只读视图。

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<const int> view = numbers;
    view[0] = 99;
    return 0;
}
```

编译器输出：

```text
span_const_write_fail.scpp:6:10: error: cannot assign to this place: it is reached through a read-only (const) reference
 6 |     view[0] = 99;
   |          ^
```

这条规则和上一节里的 `const T&` 完全一样：共享借用允许读取，但不允许写入。

## span 借用遵循与引用相同的 live 规则

第 4.2 节里的借用模型，在这里仍然适用。一个共享 span 最后一次使用结束后，同一个数组就可以开始一个可变 span 借用。

```cpp
import std;

int main() {
    int numbers[3]{};
    numbers[0] = 5;
    numbers[1] = 6;
    numbers[2] = 7;
    std::span<const int> reader = numbers;
    int first = reader[0];
    std::span<int> writer = numbers;
    writer[1] = 9;
    std::println("{} {}", first, numbers[1]);
    return 0;
}
```

输出：

```text
5 9
```

这里 `writer` 这个借用之所以被接受，是因为 `reader` 的最后一次使用已经在 `int first = reader[0];` 那一行结束了。

但如果共享 span 借用和可变 span 借用发生重叠，就会被拒绝：

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<int> writer = numbers;
    std::span<const int> reader = numbers;
    return writer[0] + reader[0];
}
```

编译器输出：

```text
span_borrow_conflict_fail.scpp:6:5: error: cannot borrow 'numbers': it is already mutably borrowed
 6 |     std::span<const int> reader = numbers;
   |     ^
```

所以，span 并不是绕过所有权检查的逃生舱。它是视图，但它依然是借用。

## 当前版本的限制

如果你想围绕 span 设计 API，今天有两个限制特别重要。

第一，构造目前仍然只接受定长数组：

```cpp
import std;

int main() {
    int value{1};
    std::span<int> view = value;
    return 0;
}
```

编译器输出：

```text
span_non_array_fail.scpp:5:27: error: std::span<T> can currently only be constructed from a fixed-size array in this version
 5 |     std::span<int> view = value;
   |                           ^
```

第二，span 目前还不能在初始化之后重新绑定：

```cpp
import std;

int main() {
    int first[2]{};
    int second[2]{};
    std::span<int> view = first;
    view = second;
    return 0;
}
```

编译器输出：

```text
span_reassign_fail.scpp:7:5: error: std::span 'view' cannot be reassigned after initialization in this version
 7 |     view = second;
   |     ^
```

所以今天的 `std::span` 更像一个“永久绑定”的借用，而不是一个可以自由重新赋值的视图值。

## `std::span` 规则小结

到这里，工作规则可以总结成：

- `std::span<T>` 是一个面向连续元素的非拥有视图；
- 构造或传递 span 时，不会复制底层元素；
- `std::span<const T>` 是只读的，而 `std::span<T>` 允许修改；
- 第 4.2 节里的借用与 live 规则，同样适用于 span；
- 今天的 span 由定长数组构造，并且构造之后不能重新绑定。

后面的数组章节还会更详细地回到缓冲区与视图这个主题。

---

[← 上一章：引用与借用](ch04-02-references-and-borrowing.md) · [目录](README.md)
