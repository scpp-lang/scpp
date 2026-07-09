# 基本构件

既然你已经见过一个完整程序，现在可以放慢一点，给其中的部件正式命名了。scpp
 是刻意从熟悉的、C++ 形状的基础积木开始的：标量值、变量、函数，以及普通的
控制流。

## 变量会记住值

变量会给一个值起名字，让你稍后还能再次使用它：

- `int total = 0;` 创建一个整数变量，并给它一个初始值；
- `total = total + 1;` 会更新这个变量；
- 一个变量在它的整个生命周期里都只有一个类型。

## 一开始先掌握三种标量类型

写前几个程序时，这三种标量类型就已经很够用了：

- `int`：整数，比如 `0`、`7`、`-42`；
- `bool`：`true` 和 `false`；
- `char`：单个字符，比如 `'A'` 或 `'x'`。

`if` 和 `while` 的条件应当是真正的 `bool` 值。也就是说，写 `score >= 20` 这
样的比较，而不是扔一个原始整数进去，指望编译器把它当成“真值”。

## 函数把可复用的工作起个名字

函数可以把一段计算包装起来，让你在多个地方调用它。它的形状对熟悉 C++ 的读
者来说会非常自然：

- 先写返回类型；
- 再写函数名；
- 然后是圆括号里的参数列表；
- 最后是花括号里的函数体。

## 一个把这些东西串起来的完整例子

下面这个完整文件把这些积木放到了一起：

```cpp
extern "C" int printf(const char* fmt, ...);
extern "C" int puts(const char* s);

int double_value(int x) {
    return x * 2;
}

int abs_diff(int left, int right) {
    if (left >= right) {
        return left - right;
    }
    return right - left;
}

bool passed(int score) {
    return score >= 20;
}

int main() {
    int total = 0;
    int i = 1;

    while (i <= 4) {
        total = total + double_value(i);
        i = i + 1;
    }

    char grade = 'A';

    [[scpp::unsafe]] {
        printf("total = %d\n", total);
        printf("grade = %c\n", grade);
        if (passed(total)) {
            puts("passed");
        } else {
            puts("keep practicing");
        }
    }

    return abs_diff(total, 20);
}
```

编译并运行：

```sh
./build/scpp basics.scpp
./a.out
```

预期输出：

```text
total = 20
grade = A
passed
```

这里有几件事值得特别留意：

- `double_value` 接收一个 `int`，返回一个 `int`；
- `abs_diff` 用 `if` 在两条路径之间做选择；
- `passed` 返回 `bool`，这个 `bool` 又被直接拿来当 `if` 条件；
- `while` 会一直重复，直到条件变成 `false`；
- `char grade = 'A';` 用的是单引号，因为它存的是一个字符，而不是一个字符串。

到这里，你已经可以读写带有普通标量状态和控制流的小型 scpp 程序了。接下来
的章节会在这个基础上继续往上搭，而不是一开始就把语言里最难的规则压到读者
面前。

## 改写期间暂时保留的参考附录

下面保留的是较早写成、偏参考手册风格的内容。后面的章节目前仍然会链接到这些
材料，所以在教程化改写分批落地期间，它们暂时继续留在这里。

这是健全性的关键，必须严格。

| 情形 | 规则 |
|------|------|
| 调用一个普通（默认受检查）函数 | 自由放行；正常参与检查，不管从哪调用——包括从 `[[scpp::unsafe]] { }` 块内部调用。 |
| 调用一个 `extern "C"` 函数 | **必须包在 `[[scpp::unsafe]] { }` 内**，否则编译错误。没有任何 scpp 编译器见过那个 C 实现、没法检查它，所以由调用方来背书。 |
| 裸指针解引用 | 必须在 `[[scpp::unsafe]] { }` 内。 |

- 边界处的数据契约：暴露给一个 `extern "C"` 函数的引用/指针，或者在
  `[[scpp::unsafe]] { }` 内部通过裸指针拿到的引用/指针，其生命周期义务过了这个
  边界就**不再强制**（`[[scpp::unsafe]] { }` 块的作者自负）。反过来，一个从
  `extern "C"` 调用**拿到**的引用/指针值，或者解引用裸指针产生的值，
  是被那个用 `[[scpp::unsafe]] { }` 背书的代码**假定为有效**的（那个块的义务）。
- 编译器需能标记某一条具体的 `extern "C"` 声明是否"已人工审核为可安全
  调用"——v0.1 不做形式化，改成每个调用点都靠 `[[scpp::unsafe]] { }` 背书。
- 机制：具体规则见 [§1.3](ch01-safety-context.md)（`[[scpp::unsafe]] { }`）。简单说：检查器会拒绝任何被调方是 `extern "C"`
  声明的 `Call`，除非调用点在词法上位于 `[[scpp::unsafe]] { }` 块内——同一个
  "当前是否在 `[[scpp::unsafe]]` 标注块里"标记，也会用来放行裸指针解引用，以及以后
  [§5.5](ch05-static-checks.md) 里其余各项语法落地后的放行。
- 没有函数体的 `extern "C"` 声明，是**无条件**这样被把关的——没有退出
  选项，因为没有任何 scpp 编译器见过它的实现、没法检查。普通 scpp 函数
  默认受检查，因此默认可以自由调用；一个函数的作者可以让**某个具体的**
  函数选择性地拿到 `extern "C"` 自动获得的同一种调用点把关待遇，办法是
  把这个函数自己的声明标记为 `[[scpp::unsafe]]`（见
  [§1.2](ch01-safety-context.md)）——跟 `extern "C"` 不一样，这是自愿的，
  而且这样标记还会额外让这个函数自己的函数体全程都是 unsafe
  context——因为（跟 `extern "C"` 不一样）scpp 编译器确实看得到、也确实
  会检查这个函数体。

## 2.1 `extern "C"` 声明

这是跟**真正的** C 打交道的边界，不只是跟不受检查的 scpp 代码打交道——
调用 libc 或任何其他 C 库。这里完全复用 C++ 现有语法，scpp 不加任何新写
法，只加额外限制和下面这套安全接线。

- **语法**：跟 C++ 现有形式完全一样——
  ```cpp
  extern "C" int printf(const char* fmt, ...);   // 单条声明
  extern "C" {                                    // 块形式：等价于给每条
      void* malloc(size_t size);                  // 声明都重复写一遍
      void free(void* p);                          // `extern "C"`，
      void abort();                                // 跟真实 C++ 一样
  }
  ```
  v0.1 只接受字面量链接字符串 `"C"`（不支持 `"C++"` 或别的）——写别的
  字符串是编译错误，报错里会说明目前只支持哪个。
- **声明 vs 定义——两者行为不一样**：
  - **没有函数体**（`extern "C" int foo(int x);`）：声明一个*在别处定义*、
    靠外部链接进来的函数。编译器看不到它的实现，所以**永远隐式不受
    检查**——没有任何办法把它标成别的（这跟 [§1.2](ch01-safety-context.md)
    的函数级 `[[scpp::unsafe]]` 标记不一样：那个标记只会给一个 scpp
    编译器本来就会检查的函数**新增**调用点把关；没有哪个 attribute 能
    **去掉** `extern "C"` 声明这条无条件把关）。调用它因此
    需要 `[[scpp::unsafe]] { }`，走的是和调用任何其他不受检查代码完全一样的机制
    （不需要任何新规则——这正是这一节的要点：`extern "C"` 只是新增了
    一个"天生不受检查"的函数签名**来源**，完全骑在
    [§1.3](ch01-safety-context.md) 已经定义好的机制上）。
  - **有函数体**（`extern "C" int add(int a, int b) { return a + b; }`）：
    定义一个普通的 scpp 函数（跟其它任何函数一样默认受检查），只是
    额外获得 C 链接，好让外部 C（或其他语言）代码能调用它。函数体和
    其他任何函数一样被完整检查；`extern "C"` 只约束**签名**的类型
    （见下）并请求 C 链接，不代表函数体本身可信与否。这跟 Rust 的
    `#[no_mangle] pub extern "C" fn foo(...)` 是一回事——签名必须
    FFI-safe，函数体照样是被正常检查的 Rust。
- **签名类型限定为 C-ABI 兼容类型**，声明和定义两种形式的每个参数和返回
  类型都要检查：标量；裸指针 `T*`（包括 `void*`——`void` 在这里成为一个
  合法的、仅用作指针指向类型的类型名；`const T*` 是独立的类型，见
  [§5.7](ch05-static-checks.md)——上面 `printf` 的 `const char* fmt`
  现在是真的只读了，不是被丢弃的限定符）；`struct`（本来就保证是
  Clang-ABI 兼容布局，见 [§4.3](ch04-struct-vs-class.md)），按值或按指针
  均可；形参位置的定长数组 `T[N]`（退化为指针，和普通 C++ 一样）。
  **被拒绝的**：`T&`/`const T&`、`std::unique_ptr`、`std::span`、
  `std::string`/`std::string_view`、`std::vector`、`std::shared_ptr`、
  `std::expected`（见 [§5.6](ch05-static-checks.md)/
  [§6](ch06-safe-subset.md)——可恢复错误同样没有对应的 C 表示），
  以及 `[[scpp::lifetime(name)]]`（没有借用检查类型可以附着，没有意义）
  ——这些都没有对应的 C 表示。一个 `extern "C"` 函数如果内部需要用
  scpp 的所有权/借用类型，就在边界上用 C 兼容的原始形式收发，进函数体后
  自己（受检查地）转换。

---

[← 上一章：第一个完整的小程序](ch01-safety-context.md) · [目录](README.md) · [继续阅读现有参考章节 →](ch03-syntactic-sugar.md)
