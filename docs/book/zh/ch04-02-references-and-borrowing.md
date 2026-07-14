# 引用与借用

所有权解释的是：谁负责清理资源。引用解释的是：代码怎样才能在**不接走这份责任**的前提下，临时使用一个值。

在 scpp 里，这里用的就是原生 C++ 引用语法：

- `const T&` 表示共享、只读借用；
- `T&` 表示可变借用。

这里没有 Rust 风格的 `&mut` 关键字。相反，scpp 会在普通 C++ 引用语法之上，再由 `movecheck` 执行受 Rust 启发的借用检查。

下面每个可运行示例都可以保存成 `references.scpp`，然后这样构建并运行：

```sh
scpp references.scpp -o references
./references
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输出，请把文件保存成诊断块里显示的那个描述性文件名。

## 用 `const T&` 借用

如果一个函数只需要“看一眼”某个值，就可以接收 `const T&`，而不是接走所有权。

```cpp
import std;

int calculate_length(const std::string& text) {
    return text.length();
}

int main() {
    std::string title{"scpp"};
    int length{calculate_length(title)};
    std::println("{} has {} bytes", title.c_str(), length);
    return 0;
}
```

输出：

```text
scpp has 4 bytes
```

调用结束后，`title` 仍然可以继续使用，因为所有权根本没有移动进 `calculate_length`。函数只是把它借用了过去。

这就是“借用”这个词的意思：被调用者只拿到临时访问权，调用者仍然是拥有者。

## 通过 `const T&` 的直接写入会被拒绝

共享借用是只读的。

```cpp
import std;

void change(const int& value) {
    value = 2;
    return;
}

int main() {
    int x{1};
    change(x);
    return 0;
}
```

编译器会拒绝它：

```text
assign_through_const_ref_fail.scpp:4:5: error: cannot assign through 'value': it is a read-only (const) reference
 4 |     value = 2;
   |     ^
```

## 用 `T&` 做可变借用

如果一个函数需要修改调用者的值、但又不想成为它的拥有者，就用 `T&`。

```cpp
import std;

void add_suffix(std::string& text) {
    text.append(" book");
    return;
}

int main() {
    std::string title{"scpp"};
    add_suffix(title);
    std::println("{}", title.c_str());
    return 0;
}
```

输出：

```text
scpp book
```

`add_suffix` 并没有接走 `title` 的所有权。它只是以可变方式借用了 `title`，原地修改之后就返回了。整个过程中，拥有者始终还是调用者。

## 任意多个共享借用，或者一个可变借用

scpp 对安全别名施加的核心限制，正是你希望看到的这个形状：

- 任意多个 `const T&` 借用可以同时存在；
- 同一时刻最多只能有一个 `T&` 借用；
- 同一个值的共享借用和可变借用不能重叠。

多个共享借用没有问题：

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    const int& second = value;
    std::println("{}", first + second);
    return 0;
}
```

输出：

```text
10
```

但如果一个共享借用还活着，再去创建同一个值的可变借用，就会被拒绝：

```cpp
import std;

int main() {
    int value{5};
    const int& first = value;
    int& second = value;
    return first + second;
}
```

编译器输出：

```text
shared_and_mut_fail.scpp:6:5: error: cannot mutably borrow 'value': it is already borrowed
 6 |     int& second = value;
   |     ^
```

同样地，第二个 `T&` 借用同一个对象也会被编译器拒绝。

这条限制真正保证的是：代码可以拥有一批共享读者，或者拥有一个可变写者，但不能两者同时存在。

## 从现有引用再借一次：reborrow

如果一个新引用**是从另一个引用形成的**，那它就是一个 reborrow。

这里能看到 scpp 现在的规则，比“整个代码块都算借用活着”这种朴素说法更精细。如果一个可变 lender 派生出了 child borrow，scpp 会按“最后一次可能使用”来判断这个 child borrow 还算不算 *live*。

只要这个 child borrow 还 live：

- 仍然允许通过 lender 去读；
- 不允许通过 lender 去写；
- 不允许再从同一个 lender 形成新的 reborrow。

通过 lender 去读是可以的：

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    std::println("{}", lender + child);
    return 0;
}
```

输出：

```text
10
```

但如果 child 还 live，就不能再通过 lender 去写：

```cpp
import std;

int main() {
    int value{1};
    int& first = value;
    int& second = first;
    first = 2;
    std::println("{} {}", first, second);
    return 0;
}
```

编译器输出：

```text
reborrow_write_lender_fail.scpp:7:5: error: cannot write through 'first' while a nested reborrow derived from it is still live
 7 |     first = 2;
   |     ^
```

类似地，如果此时再写 `int& third = first;`，也会被拒绝：

```text
reborrow_further_reborrow_fail.scpp:7:5: error: cannot form another reborrow from 'first' while a nested reborrow derived from it is still live
 7 |     int& third = first;
   |     ^
```

一旦 child borrow 的最后一次使用已经过去，lender 就会重新变得可写；即使整个代码块还没结束，也是如此：

```cpp
import std;

int main() {
    int value{5};
    int& lender = value;
    const int& child = lender;
    int before = child;
    lender = 7;
    int& second = lender;
    second = second + 1;
    std::println("{} {}", before, second);
    return 0;
}
```

输出：

```text
5 8
```

这里 `lender = 7;` 能通过，是因为 `child` 的最后一次使用已经在 `int before = child;` 那一行发生完了。scpp 检查的是“最后一次使用之后是否还活着”，而不只是“词法作用域有没有结束”。

## 引用必须始终有效

引用绝不能比它所指向的对象活得更久。

今天的 scpp v0.1 会用一种偏保守的方式在函数返回处执行这条规则：如果函数返回引用，编译器就必须能够把这个返回引用的生命周期，推断为来自某个输入引用形参。因此，函数不能从一个局部对象里“现造一个引用”再把它返回出去。

```cpp
import std;

const std::string& bad() {
    std::string local{"oops"};
    return local;
}

int main() {
    const std::string& ref = bad();
    std::println("{}", ref.c_str());
    return 0;
}
```

编译器输出：

```text
return_local_ref_fail.scpp:3:1: error: function 'bad' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- refactor to take a single reference parameter, or return by value/std::unique_ptr instead
 3 | const std::string& bad() {
   | ^
```

最直接的修正方式，就是把拥有的值本身返回出去：

```cpp
import std;

std::string make_title() {
    std::string local{"scpp"};
    return local;
}

int main() {
    std::string title{make_title()};
    std::println("{}", title.c_str());
    return 0;
}
```

输出：

```text
scpp
```

## 引用规则小结

到这里，工作规则可以总结成：

- `const T&` 会借用一个值而不接走所有权，并且直接通过它写入会被拒绝；
- `T&` 同样是在借用、不接走所有权，但它允许修改；
- 一个值要么同时拥有任意多个共享借用，要么同时只有一个可变借用；
- live 的 reborrow 会阻止经由 lender 写入，也会阻止继续从 lender 形成新的 reborrow，但不会阻止经由 lender 读取；
- reborrow 的 live 范围由最后一次使用决定，而不只由代码块结尾决定；
- 引用必须始终有效，而今天的 scpp 只有在能从输入引用推断生命周期时，才接受“返回引用”。

下一节会把同一套借用模型，应用到“面向一段元素范围的非拥有视图”上。

---

[← 上一章：什么是所有权？](ch04-01-what-is-ownership.md) · [目录](README.md)
