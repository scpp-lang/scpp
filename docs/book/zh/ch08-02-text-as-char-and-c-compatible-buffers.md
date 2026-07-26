# 把文本当作 `char` 与 C 兼容缓冲区处理

第 8.1 节先把“定长数组”这件事本身讲清楚了。这一节把同样的思路收窄到 `char`:
字符串字面量、原始 C 字符串,以及把字节直接存在自己内部的可写缓冲区。

这种底层表示在今天的 scpp 里依然很重要。`std::string` 已经存在,而且在你需要一
个可增长、自己拥有的文本值时很好用;但很多 API -- 尤其是 `extern "C"` 边界 --
今天仍然直接使用 `const char*` 和 `char[N]`。

对下面每个可以运行的例子,把文件保存成 `char-buffers.scpp`,然后这样编译运行:

```sh
scpp char-buffers.scpp -o char-buffers
./char-buffers
```

对于那些本来就应该被拒绝的例子,如果你想让编译器输出逐字匹配,就按诊断块里给
出的描述性文件名保存。

## 字符串字面量是只读的 `const char*`

字符串字面量给你的是一段借用来的只读文本。

```cpp
import std;

int main() {
    const char* greeting = "hello";
    std::println("{} {}", greeting, greeting[1]);
    return 0;
}
```

输出:

```text
hello e
```

`greeting` 并不拥有一块由 `main` 自己分配出来的缓冲区。它只是一个指向现成只读字
节的指针,而对这个指针做下标访问,读到的就是其中单个字符。

## `char[N]` 数组会把可写文本缓冲区直接拥有在自己内部

当你需要一块可以由当前函数自己修改的字节存储时,定长 `char` 数组就跟第 8.1 节
里的其它数组一样工作。额外要记住的一条规则是:C 风格文本必须以 `\0` 结尾,这样
另一个 API 才知道文本在哪里结束。

```cpp
extern "C" int puts(const char* s);

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '!';
    word[5] = '\0';

    [[scpp::unsafe]] {
        puts(word);
    }
    return 0;
}
```

输出:

```text
scpp!
```

`word` 直接拥有六个 `char` 元素。对 `puts` 的调用要放进 `[[scpp::unsafe]]` 里,
原因跟本书里其它 `extern "C"` 调用一样:编译器看不到对面真正的实现。

最后那个 `\0` 和前面的可见字符一样重要。`puts` 会一直往后读,直到碰到这个终止
符为止。

## 把 `char[N]` 缓冲区传给函数时,改到的仍然是同一块存储

在调用边界上,数组缓冲区可以传给一个写成数组语法的参数,而被调函数会直接原地改
写调用方那块缓冲区。

```cpp
import std;

void make_excited(char text[6]) {
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

    make_excited(word);
    const char* text = word;
    std::println("{}", text);
    return 0;
}
```

输出:

```text
scpp!
```

`make_excited` 并没有收到一个新拷贝出来的数组。它写到的仍然是同一块底层缓冲
区,所以调用方里的 `word[4]` 也跟着变了。

## `std::string` 负责拥有可增长文本,`c_str()` 负责接到 C API 上

当大小一开始就确定时,定长 `char[N]` 缓冲区很好用。要是你需要一个自己拥有、并
且还能增长的文本值,就用 `std::string`,只在真正需要的边界上借出 C 兼容视图。

```cpp
import std;

extern "C" int puts(const char* s);

int main() {
    std::string name{"scpp"};
    name.append(" book");

    [[scpp::unsafe]] {
        puts(name.c_str());
    }

    int letters = static_cast<int>(name.size());
    std::println("{}", letters);
    return 0;
}
```

输出:

```text
scpp book
9
```

这里文本的所有权一直都在 `name` 手里,不在那个 C API 手里。`c_str()` 给出的是
`puts` 需要的、借用来的 nul 结尾指针,而在普通 scpp 代码里,`std::string` 仍然是
处理动态长度文本时更合适的工具。

## 字符串字面量不能直接初始化可变 `char*`

字面量本来就是只读文本,所以 scpp 不允许你把它当成可写缓冲区。

```cpp
int main() {
    char* text = "hi";
    return 0;
}
```

编译器输出:

```text
string_literal_mutable_pointer_fail.scpp:2:18: error: cannot initialize or assign raw pointer 'text' from an incompatible pointer type without an explicit cast
```

如果一段代码打算通过这个指针写数据,那它就必须真的拿到可写存储 -- 比如它自己拥
有的 `char[N]` 数组。如果只是读取字面量,那就改用 `const char*`。

## 到目前为止,文本缓冲区的规则是

- 字符串字面量是借用来的只读文本,通常写成 `const char*`;
- 可写的 C 风格文本缓冲区通常是一个定长 `char[N]` 数组,并且要显式写上结尾的
  `\0`;
- 把这个数组传给别的函数时,对方拿到的是对同一组底层字节的指针式访问;
- 当文本需要增长时,`std::string` 是更合适的拥有型工具,而 `c_str()` 可以把一个
  借用来的 C 兼容指针交给别的 API;
- 字符串字面量不能直接初始化可变 `char*`。

下一节会回到“借用视图”这个主题,用 `std::span` 来表达“看着一块连续缓冲区,但并
不接管它的所有权”。

---

[← 上一章：定长数组](ch08-01-fixed-size-arrays.md) · [目录](README.md) · [下一章：用 `std::span` 借用视图 →](ch08-03-borrowed-views-with-std-span.md)
