# 定长数组

上一节用“把一个模块拆到多个文件里”结束了第 7 章。这一章把焦点从项目布局拉回
到普通数据本身:定长数组、字符缓冲区,以及对连续存储做借用视图。

定长数组会把一组已知数量的元素直接存放在自己内部。它自己拥有这些值,而它的长
度也是类型的一部分。当你一开始就知道自己需要多少个元素时,数组就是 scpp 目前
最简单的序列类型。

对下面每个可以运行的例子,把文件保存成 `arrays.scpp`,然后这样编译运行:

```sh
scpp arrays.scpp -o arrays
./arrays
```

对于那些本来就应该被拒绝的例子,如果你想让编译器输出逐字匹配,就按诊断块里给
出的描述性文件名保存。

## 用固定元素个数声明数组

`T[N]` 的意思是“一个元素类型为 `T`、长度为 `N` 的数组”。

```cpp
import std;

int main() {
    int scores[4]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    scores[3] = 40;

    std::println("{} {}", scores[0], scores[3]);
    return 0;
}
```

输出:

```text
10 40
```

`scores` 直接拥有四个 `int` 值。这里的 `[4]` 不是只给人看的注释;它本身就是变
量类型的一部分,每一个合法下标都必须落在这个固定长度里。

空花括号也很重要。就今天的 scpp 来说,`T[N]{}` 是最稳妥的起点:先把整个数组初
始化好,然后再把你想要的元素一个个填进去。

## 用 `auto&` 做 range-based `for` 可以原地修改每个元素

第 3.5 节已经用过 range-based `for` 来依次读取数组元素。把循环变量写成
`auto&` 时,拿到的就是每个元素的可变引用。

```cpp
import std;

int main() {
    int values[3]{};
    int next = 1;

    for (auto& value : values) {
        value = next * 10;
        next = next + 1;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

输出:

```text
10
20
30
```

第一层循环里的每个 `value` 都引用着数组里的真实元素,不是一份拷贝。通过这个
引用写入,改到的就是数组自己。

## 数组长度也可以来自前面定义的 `constexpr`

长度不一定非得手写成字面量。只要是前面已经可用、并且能解析成正整数长度的常量
表达式,也一样可以。

```cpp
import std;

int main() {
    constexpr int count = 4;
    int values[count]{};
    values[3] = 9;

    std::println("{} {}", count, values[3]);
    return 0;
}
```

输出:

```text
4 9
```

这样通常比到处重复同一个字面量更清楚。关键不在于这个名字恰好叫 `count`,而在
于它是个 `constexpr`,所以编译器能在程序运行前就把数组长度算出来。

## 数组长度必须是常量表达式

普通的运行期变量不能拿来当数组长度。

```cpp
int main() {
    int count = 4;
    int values[count]{};
    return 0;
}
```

编译器输出:

```text
array_nonconst_bound_fail.scpp: error: 3:16: expression is not a constant expression: identifier 'count' is not available
```

这里的 scpp 不支持“运行期才知道长度的局部数组”。编译器必须一开始就知道
`values` 的完整大小,所以这个长度必须在编译阶段就能确定,不能等到运行时再发现。

## 明显越界的常量下标会被立刻拒绝

因为长度本来就是类型的一部分,所以只要一个常量下标明显不可能合法,编译器就能
直接在编译期拒绝它。

```cpp
int main() {
    int values[3]{};
    return values[3];
}
```

编译器输出:

```text
array_out_of_bounds_compile.scpp:3:12: error: array subscript 3 is out of bounds for array of size 3
 3 |     return values[3];
   |            ^
```

对 `int values[3]` 来说,合法下标只有 `0`、`1`、`2`。既然源码里直接写出了 `3`,编
译器就已经有足够的信息在程序运行前把它拦下来。

## 今天请按元素逐个填值,不要直接写一个值列表

在普通 C++ 里,`int scores[4]{10, 20, 30, 40};` 会是第一节那个例子的自然写法。
但在今天的 scpp 里,这种“多个元素一起写在花括号里”的初始化还不支持。

```cpp
int main() {
    int scores[4]{10, 20, 30, 40};
    return 0;
}
```

编译器输出:

```text
array_brace_init.scpp:2:5: error: brace-initialization of this member requires exactly one expression
 2 |     int scores[4]{10, 20, 30, 40};
   |     ^
```

这也就是为什么本节所有可运行示例都先从 `T[N]{}` 开始,再把元素一个个赋值进去。

## 到目前为止,定长数组的规则是

- `T[N]` 声明“恰好有 `N` 个 `T` 元素”的数组;
- 数组直接拥有这些元素,而 `N` 本身就是类型的一部分;
- 用 `auto&` 做 range-based `for` 可以原地修改每个元素;
- 长度可以来自前面定义的 `constexpr`,但不能来自普通运行期变量;
- 明显越界的常量下标会在编译期被拒绝;
- 今天最稳妥的写法是先用 `{}` 做零初始化,再显式填每个元素。

下一节仍然会继续讲数组,不过会把元素类型收窄到 `char`,把这块定长存储当成一个
跟 C 兼容的文本缓冲区来用。

---

[← 上一章：把模块拆到不同文件中](ch07-05-separating-modules-into-different-files.md) · [目录](README.md)
