# 如何把“信任”局部化到真实程序里

前两节解释了 `[[scpp::unsafe]]` 到底给什么开门，以及裸指针、`extern "C"` 调用是
如何跨过这道边界的。

这一节要回答下一个更实际的问题：**在真实程序里，unsafe 代码应该放在哪里？**

总规则其实很简单：

- 让 unsafe 区域尽可能小；
- 如果一个函数能完全为自己的 unsafe 操作负责，就优先写成普通安全函数，在内部
  放一个很小的 unsafe block；
- 只有当函数的正确性依赖于调用者必须满足、而函数体自己无法证明的前提条件时，
  才使用函数级别的 `[[scpp::unsafe]]`。

下面每个可运行示例都可以保存成 `trust.scpp`，然后这样构建并运行：

```sh
scpp trust.scpp -o trust
./trust
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输
出，请把文件保存成诊断块里显示的那个描述性文件名。

## 优先在普通安全函数里放一个很小的 unsafe block

如果一个函数本身就能完全控制并证明那次 unsafe 操作是合理的，那么就让这个函数
保持普通，只把真正关键的那一步放进 unsafe。

```cpp
import std;

int first_of_pair(int left, int right) {
    int values[2]{};
    values[0] = left;
    values[1] = right;
    int* pointer = &values[0];
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    std::println("{}", first_of_pair(11, 22));
    return 0;
}
```

输出：

```text
11
```

这里，调用者根本不需要知道裸指针的存在。这个函数自己创建了局部数组、形成了指
针，也只在一个很小的位置解引用了它，所以它可以直接为那一步负责。

## 当调用者必须担保时，使用函数级别的 `[[scpp::unsafe]]`

有时候，函数体自己无法独立让操作变得可靠。如果函数接收的是外部给来的裸指针，
那么它的正确性就取决于一个只有调用者才能保证的前提条件。

```cpp
import std;

[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    [[scpp::unsafe]] {
        std::println("{}", read_first(&value));
    }
    return 0;
}
```

输出：

```text
9
```

这里的 unsafe，不再只是一个内部实现细节，而是这个函数契约的一部分。

## 这个契约会传播到调用点

因为真正需要担保输入的人是调用者，所以从安全代码里直接调用这种函数，会被编译
器拒绝。

```cpp
[[scpp::unsafe]] int read_first(int* pointer) {
    return *pointer;
}

int main() {
    int value{9};
    return read_first(&value);
}
```

编译器输出：

```text
call_unsafe_wrapper_outside_unsafe_fail.scpp:7:12: error: cannot call 'read_first' outside '[[scpp::unsafe]] { }': its own declaration is marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the caller can guarantee (ch01 §1.2/§1.3)
 7 |     return read_first(&value);
   |            ^
```

所以，函数级别的 `[[scpp::unsafe]]` 应该被看成一个很明确的 API 设计决定。它会让
调用者也分担安全论证的责任。

## 即使在较大的封装里，也要把 unsafe 边界压窄

真实程序里，往往需要连续调用几个外部函数，但规则仍然一样：让每个 unsafe 区域
尽量贴近真正需要它的那一次调用，让其余逻辑继续保持普通安全代码。

```cpp
import std;

extern "C" {
    int socket(int domain, int type, int protocol);
    int getsockopt(int fd, int level, int optname, void* optval, int* optlen);
    int close(int fd);
}

int query_socket_type() {
    int fd = 0;
    [[scpp::unsafe]] {
        fd = socket(2, 2, 0);
    }

    int value = 0;
    int len = 4;
    [[scpp::unsafe]] {
        getsockopt(fd, 1, 3, &value, &len);
        close(fd);
    }
    return value;
}

int main() {
    std::println("{}", query_socket_type());
    return 0;
}
```

输出：

```text
2
```

`query_socket_type` 的大部分代码仍然是普通代码：局部变量、返回值和控制流都没有
变。真正被围起来的，只有那些外部调用本身。

## 即使整个 unsafe 区域很大，检查器也仍然开着

把整个代码块标成 unsafe，**并不**意味着关闭所有权检查。即使有时你确实需要一个
较宽的 unsafe 区域，scpp 仍然会继续检查 move 和 borrow。

```cpp
import std;

int f() {
    [[scpp::unsafe]] {
        std::unique_ptr<int> first = std::make_unique<int>(1);
        std::unique_ptr<int> second = std::move(first);
        std::unique_ptr<int> third = std::move(first);
        return *third;
    }
}

int main() {
    return f();
}
```

编译器输出：

```text
unsafe_whole_body_still_checks_moves_fail.scpp:7:38: error: use of moved-out variable 'first'
 7 |         std::unique_ptr<int> third = std::move(first);
   |                                      ^
```

这才是“把信任局部化”的真正含义：unsafe 的部分应该只包含那些编译器确实无法自行
证明的地方，其余部分仍然应该尽量留在普通受检查的世界里。

下一章会离开这些底层边界，转向项目结构：包、模块与清单文件。

---

[← 上一章：调用 `extern "C"` 与使用裸指针](ch06-02-calling-extern-c-and-using-raw-pointers.md) · [目录](README.md)
