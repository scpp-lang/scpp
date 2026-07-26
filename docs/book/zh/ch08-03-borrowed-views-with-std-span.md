# 用 `std::span` 借用视图

第 4.3 节是从“所有权”的角度引入 `std::span` 的:span 是对一段连续元素的借用,
不是新的拥有者。现在这一章已经把数组和字符缓冲区摆在桌面上了,就可以再回过头
来,把 span 看成“某个现有 `T` 缓冲区”的标准函数边界写法。

定长数组回答的是“元素存在哪里?” span 回答的是“另一个函数怎样在不拷贝、也不
接管所有权的前提下使用同一批元素?”

对下面每个可以运行的例子,把文件保存成 `span-buffers.scpp`,然后这样编译运行:

```sh
scpp span-buffers.scpp -o span-buffers
./span-buffers
```

对于那些本来就应该被拒绝的例子,如果你想让编译器输出逐字匹配,就按诊断块里给
出的描述性文件名保存。

## 一个 `std::span<const T>` 参数就能读取不同长度的数组

借用视图让同一个函数可以接收任意长度的定长数组,只要元素类型一致就行。

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
    int first[3]{};
    first[0] = 10;
    first[1] = 20;
    first[2] = 30;

    int second[5]{};
    second[0] = 1;
    second[1] = 2;
    second[2] = 3;
    second[3] = 4;
    second[4] = 5;

    std::println("{} {}", sum(first), sum(second));
    return 0;
}
```

输出:

```text
60 15
```

这两个调用都没有拷贝数组元素。每次调用都只是在边界处构造出一个小小的只读视
图,然后 `sum` 通过这个视图去读数据。

## 可变 span 可以原地改写借用来的 `char` 缓冲区

上一节用 `char[N]` 构建了和 C 兼容的文本缓冲区。如果某个辅助函数需要直接修改那
块现有缓冲区,它就可以接收 `std::span<char>`。

```cpp
import std;

void shout(std::span<char> text) {
    text[0] = 'S';
    text[4] = '!';
    return;
}

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '?';
    word[5] = '\0';

    shout(word);
    const char* view = word;
    std::println("{}", view);
    return 0;
}
```

输出:

```text
Scpp!
```

`shout` 并没有得到这块缓冲区的所有权。它只是借用了这六个现成字符,并通过这次
借用直接写了进去。

## span 会把当前借用缓冲区的长度一起带上

和单独一个裸指针不同,span 会把元素个数和数据本身一起带着走。这样在写循环时,
它就更适合用来保证访问不会跑出借用缓冲区的范围。

```cpp
import std;

int count_visible(std::span<const char> text) {
    int used = 0;
    while (used < text.size && text[used] != '\0') {
        used = used + 1;
    }
    return used;
}

int main() {
    char title[8]{};
    title[0] = 'b';
    title[1] = 'o';
    title[2] = 'o';
    title[3] = 'k';
    title[4] = '\0';

    std::println("{}", count_visible(title));
    return 0;
}
```

输出:

```text
4
```

这里可见文本在第一个 `\0` 就结束了,但它借用的那块缓冲区本身仍然有八个元素。
`std::span<const char>` 让函数能同时尊重这两件事:既能在文本终止符处停下,也不会
越过真实数组的边界继续读。

## span 不能活得比它借用的存储更久

因为 span 只是一个视图,所以如果你试图返回一个指向局部数组的 span,程序会被拒绝。

```cpp
std::span<int> bad() {
    int local[2]{};
    return local;
}

int main() {
    return 0;
}
```

编译器输出:

```text
span_return_local_fail.scpp:3:5: error: function 'bad' returns a lifetime-tracked value from an incompatible source type
```

这仍然是前面那个所有权故事,只不过现在应用到了缓冲区视图上:局部数组一旦死掉,
指向它的 span 也会立刻悬空,所以 scpp 会直接拒绝这段程序。

## 到目前为止,在面向缓冲区的代码里使用 span 的规则是

- 让数组和 `char[N]` 缓冲区继续做自己那块存储的拥有者;
- 当函数只需要读取一段现有连续缓冲区时,用 `std::span<const T>`;
- 当函数需要原地修改那段借用缓冲区时,用 `std::span<T>`;
- span 会同时带着起始地址和当前元素个数;
- span 依然服从所有权和生命周期规则,所以它不能活得比自己借用的存储更久。

有了这一章里的数组、文本缓冲区和 span,我们就具备了今天 scpp 里处理连续数据的
基础词汇:拥有型的内联存储、和 C 兼容的字符缓冲区,以及覆盖在两者之上的借用视图。

---

[← 上一章：把文本当作 `char` 与 C 兼容缓冲区处理](ch08-02-text-as-char-and-c-compatible-buffers.md) · [目录](README.md) · [下一章：不可恢复错误与编译器插入的检查 →](ch09-01-unrecoverable-errors-and-compiler-inserted-checks.md)
