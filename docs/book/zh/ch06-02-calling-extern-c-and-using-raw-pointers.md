# 调用 `extern "C"` 与使用裸指针

上一节介绍了 `[[scpp::unsafe]]`：它是 scpp 用来打开少数几个明确未受检查操作的窄
门。

其中最常见的两类，就是：

- 调用一个没有函数体的 `extern "C"` 函数；
- 解引用一个裸指针。

这一节就专门聚焦在这两类操作上，以及即使在 unsafe 代码里也依然成立的那一点点
类型信息。

下面每个可运行示例都可以保存成 `raw-pointers.scpp`，然后这样构建并运行：

```sh
scpp raw-pointers.scpp -o raw-pointers
./raw-pointers
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输
出，请把文件保存成诊断块里显示的那个描述性文件名。

## 形成裸指针本身是安全的

用 `&value` 取地址，本身属于普通安全代码。真正需要 `[[scpp::unsafe]]` 的，是
*信任* 这个指针并解引用它。

```cpp
import std;

int main() {
    int value{1};
    int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 9;
    }
    std::println("{}", value);
    return 0;
}
```

输出：

```text
9
```

这种分工是刻意设计的。安全代码可以先为底层 API 准备好裸指针，但真正的解引用仍
然要放在一个明确的 unsafe 边界之后。

## 不带 unsafe 的裸指针解引用会被拒绝

如果你试图在安全代码里直接解引用裸指针，编译器就会拦下你。

```cpp
int read_value(int* pointer) {
    return *pointer;
}

int main() {
    int value{42};
    return read_value(&value);
}
```

编译器输出：

```text
raw_pointer_unsafe_fail.scpp:2:12: error: cannot dereference raw pointer 'pointer': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)
 2 |     return *pointer;
   |            ^
```

## 调用一个没有函数体的 `extern "C"` 函数

没有函数体的 `extern "C"` 声明，也是另一种未受检查边界。scpp 看不到它的实现，
因此调用它同样需要 unsafe context。

```cpp
import std;

extern "C" int abs(int x);

int main() {
    [[scpp::unsafe]] {
        std::println("{}", abs(-7));
    }
    return 0;
}
```

输出：

```text
7
```

这里的设计模式和裸指针是同一个：声明这个边界本身没有问题，但真正去信任它时，
就必须写出 `[[scpp::unsafe]]`。

## 在安全代码里调用这个 `extern "C"` 函数会被拒绝

如果这次调用发生在 unsafe context 之外，编译器就会拒绝它。

```cpp
extern "C" int abs(int x);

int main() {
    return abs(-7);
}
```

编译器输出：

```text
calling_extern_c_requires_unsafe_fail.scpp:4:12: error: cannot call 'extern "C"' function 'abs' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real implementation to check it (spec ch01 §1.3/ch02)
 4 |     return abs(-7);
   |            ^
```

## 可变指针可以扩宽成 `const` 指针

普通的指针类型规则依然成立。一个可变的 `T*`，可以传给需要 `const T*` 的地方。

```cpp
import std;

int read(const int* pointer) {
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    int value{7};
    int* pointer = &value;
    std::println("{}", read(pointer));
    return 0;
}
```

输出：

```text
7
```

所以，`[[scpp::unsafe]]` 并不会抹掉类型系统。它只会给某些特定操作开门。

## 即使在 unsafe block 里，通过 `const` 指针写入仍然是类型错误

哪怕放进 unsafe block，`const int*` 也仍然是只读的。

```cpp
int main() {
    int value{5};
    const int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 10;
    }
    return value;
}
```

编译器输出：

```text
write_through_const_pointer_fail.scpp:5:9: error: cannot assign to this place: it is reached through a read-only (const) reference
 5 |         *pointer = 10;
   |         ^
```

## 对只读位置取地址会得到 `const T*`

同一条规则在“形成指针”时也会出现。如果来源位置只能通过 `const` 到达，那么得到的
指针类型就必须是 `const T*`，而不能是 `T*`。

```cpp
int read(const int& value) {
    int* pointer = &value;
    return 0;
}

int main() {
    int number{1};
    return read(number);
}
```

编译器输出：

```text
address_of_const_ref_fail.scpp:2:20: error: cannot assign '&' of a read-only-reachable place to 'pointer' (a mutable 'T*'): would need 'const T*', which 'pointer' isn't declared as
 2 |     int* pointer = &value;
   |                    ^
```

所以，scpp 里的裸指针虽然是底层工具，但它们并不是“无类型”的。一个指针是可变还
是 `const`，在任何地方都依然重要。

下一节会继续停留在 unsafe 这一章，但会把重点从单个调用、单次解引用的机制，转
到“在真实程序里如何把信任局部化”这个更大的问题上。

---

[← 上一章：`[[scpp::unsafe]]` 会做什么、不会做什么](ch06-01-what-scpp-unsafe-does-and-does-not-do.md) · [目录](README.md)
