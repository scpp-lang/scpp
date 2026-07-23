# 使用 `import` 与限定名

自从引入模块以来,每一个例子都手动配对着同样两件事:一句给模块命名的
`import`,以及在每一个调用处完整写出来的、用 `::` 限定的路径,用来访问其中的
内容。这一节单独研究 `import` 本身——它到底接受哪些形式,它到底会不会、以及
如何,把名字带入作用域,还有它和一个路径自己的限定名字究竟是怎么配合的。

下面的每一个例子都放在同一个包里。

`scpp.toml`:

```toml
manifest-version = 1

[package]
name = "mathlib-app"
version = "0.1.0"

[[bin]]
name = "app"
sources = ["src/*.scpp"]
```

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## `import` 命名的是整个模块——从来不是其中单独一个条目

到目前为止,每一句 `import` 的形状都完全一样:关键字、一个点分名字、一个分
号。既然路径已经用 `::` 来访问模块内部某一个具体的条目,很自然会让人好奇
`import` 是不是也接受同样的东西——比如说,只从 `mathlib` 里导入
`sum_of_squares`,而不是导入整个模块。

`src/mathlib.scpp`:

```cpp
export module mathlib;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib::sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

编译器输出:

```text
src/main.scpp:2:15: error: expected ';' but found '::'
 2 | import mathlib::sum_of_squares;
   |               ^
```

`::` 完全不会出现在一句 `import` 里——它只会出现在之后,出现在调用处的路径
里。换成一个点,模仿模块自己多段名字的写法,倒是能通过解析:

```cpp
import std;
import mathlib.sum_of_squares;

int main() {
    std::println("{}", sum_of_squares(3, 4));
    return 0;
}
```

编译器输出:

```text
src/main.scpp: error: cannot find module 'mathlib.sum_of_squares' (use --import mathlib.sum_of_squares=path/to/file or -I <dir>)
```

这一次是因为别的原因才失败的。[在模块树中引用条目的路径]
(ch07-03-paths-for-referring-to-items-in-module-tree.md) 说明过,一个点会把
模块自己的名字连接成若干段,就像 `mathlib.trig` 那样。这里适用的是同一条规
则:`mathlib.sum_of_squares` 会被理解成一个有两段的模块名字,`mathlib` 和
`sum_of_squares`——而不是“模块 `mathlib` 里面那个叫 `sum_of_squares` 的条
目”——而且根本不存在这样一个模块。`import` 里的点永远是另一段模块名字,从来
不是条目选择符。`import` 没有任何“局部导入”的形式:每一句 `import` 要么完
整地命名一个模块,要么什么都命名不了。

## 导入一个模块,并不会把它的名字不加限定地带入作用域

既然 `import mathlib;` 是依赖 `mathlib` 的唯一方式,它到底把什么带入了作用
域?从第一章开始的每一个例子其实早就回答了这个问题,只是从来没有被明确指出
来:`import std;` 从来没有让后面哪一行直接裸调用 `println`——一直都是
`std::println`。任何其他模块也是同样的道理。

```cpp
import std;
import mathlib;

int main() {
    return sum_of_squares(3, 4);
}
```

编译器输出:

```text
src/main.scpp:5:12: error: call to unknown function 'sum_of_squares'
 5 |     return sum_of_squares(3, 4);
   |            ^
```

`mathlib` 确实被导入了,`sum_of_squares` 也确实从中被导出了,但裸名字
`sum_of_squares` 在这里和一个从未被声明过的名字一样,完全无法识别。只有限定
形式才能访问到它:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

输出:

```text
25
```

去掉 `println` 前面的 `std::`,失败的原因和方式完全一样——`std` 和
`mathlib` 一样,也只是一个被 `import` 带入作用域的普通模块:

```cpp
import std;

int main() {
    println("{}", 42);
    return 0;
}
```

编译器输出:

```text
src/main.scpp:4:5: error: call to unknown function 'println'
 4 |     println("{}", 42);
   |     ^
```

`import` 只会让一个模块导出的内容可以通过它们各自完整的路径访问到。它从来
不会缩短这条路径,也从来不会把模块的任何名字单独带入作用域——标准库也不例
外。

## 同样的规则适用于每一个被导出的条目,不只是函数

到目前为止,以这种方式访问到的名字都是函数,但这条规则并不是针对函数的。它
对一个 `struct` 同样适用。

`src/mathlib.scpp`,在 `sum_of_squares` 旁边加上一个 `Point`:

```cpp
export module mathlib;

namespace mathlib {
    export struct Point {
        int x;
        int y;
    };

    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    mathlib::Point p{};
    p.x = 3;
    p.y = 4;
    std::println("{}", mathlib::sum_of_squares(p.x, p.y));
    return 0;
}
```

输出:

```text
25
```

`Point` 是在 `mathlib::Point` 这个它自己完整的限定名下构造出来的,和
`sum_of_squares` 在 `mathlib::sum_of_squares` 下被调用是同一回事。裸写
`Point` 在这里和裸写 `sum_of_squares` 在上一节里一样,都不在作用域内——
`import` 把它们两个带入作用域的方式完全一样,也就是说:只能通过各自完整的
路径,而与条目本身是什么种类无关。

## 一个文件里的每一句 `import` 都必须出现在其他内容之前

到目前为止,不管是这一节还是前两节,每一句 `import` 都出现在自己所在文件的
最前面,在任何其他声明之前。这并不是一种风格选择。

```cpp
import std;

int triple(int x) {
    return x * 3;
}

import std;

int main() {
    std::println("{}", triple(4));
    return 0;
}
```

编译器输出:

```text
src/main.scpp:7:1: error: expected a type name
 7 | import std;
   | ^
```

第二句 `import std;` 被拒绝了——并不是因为重复导入 `std` 本身有什么问题,
而是因为它所在的位置。一旦解析越过了文件最前面那一整段连续的 `import` 和
`export import`,`import` 就不再被识别为任何内容的开头。一个文件需要的所有
`import`,都必须集中写在其他声明之前。

## 普通 `import` 和 `export import` 决定的始终只是谁能访问一个名字

[用模块控制作用域与可见性](ch07-02-control-scope-and-privacy-with-modules.md)
已经讲过两者的区别:普通的 `import name;` 只对写下它的文件私有,而
`export import name;` 会把 `name` 自身的导出内容,以它们各自原本的名字,传
递式地重新导出给转而导入当前模块的任何文件。这个区别原样成立,和那里描述的
完全一样——这里新加进来要检验的,是一个自身名字有不止一段的模块。

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/stats.scpp`,用普通、私有的方式导入它:

```cpp
export module stats;

import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`,只导入 `stats`:

```cpp
import std;
import stats;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

编译器输出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::sin_deg'
 5 |     return mathlib::trig::sin_deg(30);
   |            ^
```

和单段模块名字的情形完全一样,`stats.scpp` 自己那句普通的
`import mathlib.trig;` 不会把 `mathlib::trig::sin_deg` 转发给任何转而导入
`stats` 的文件。把这一行改成 `export import mathlib.trig;`,结果就不一样了:

`src/stats.scpp`:

```cpp
export module stats;

export import mathlib.trig;

namespace stats {
    export int double_sin_deg(int x) {
        return mathlib::trig::sin_deg(x) * 2;
    }
}
```

`src/main.scpp`,依然只导入 `stats`:

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    std::println("{}", stats::double_sin_deg(30));
    return 0;
}
```

输出:

```text
30
60
```

`mathlib::trig::sin_deg` 传到 `main.scpp` 时,用的仍然是它原本的两段
路径——重新导出一个多段模块,并不会改变它的路径需要多少段,或者每一段叫什
么。普通 `import` 和 `export import` 决定的始终只是哪些文件能沿着一条路径
访问到内容;两者都不会改变路径本身。

## `import` 没有别名机制

C++ 可以用 `namespace alias = long::qualified::name;` 把一个很长的限定名字
绑定到一个更短的名字上。scpp 的 `import` 没有类似的东西。

```cpp
import std;
import mathlib as m;

int main() {
    return m::sum_of_squares(3, 4);
}
```

编译器输出:

```text
src/main.scpp:2:16: error: expected ';' but found 'as'
 2 | import mathlib as m;
   |                ^
```

`import` 只有一种形式:关键字、一个点分模块名字、一个分号——前面可以选择性
地加上 `export`。没有任何东西能在导入的同时给模块改名,导入之后也没有任何
东西能缩短这条路径。这一节以及前两节里的每一个调用处,写出来的都是同一条完
整路径——模块自己的名字和命名空间已经决定好的那一条,每一次都原样写出。

## 到目前为止关于 `import` 与限定名的规则

- 一句 `import` 总是完整地命名一整个模块——没有办法只导入其中一个条目,
  `::` 也从不出现在 `import` 这一行本身里,只会出现在之后的路径里;
- 导入一个模块,无论对哪种条目,都不会把它的任何名字不加限定地带入作用
  域——访问它们始终需要各自完整的路径;
- 一个文件里的每一句 `import` 都必须出现在其他任何声明之前;
- 普通 `import` 只让一个名字在写下它的文件内部可以访问;`export import` 依
  然会转发同一个名字,路径不变,不管它有多少段;
- scpp 对导入或者限定名都没有别名机制——每一个调用处写出来的,都是模块自己
  早已选定的那一条路径。

到目前为止,一个模块始终对应着恰好一个文件。下一节要看看,一旦一个模块自己
的源码需要分散到不止一个文件里,哪些东西会变,哪些不会。

---

[← 上一章：在模块树中引用条目的路径](ch07-03-paths-for-referring-to-items-in-module-tree.md) · [目录](README.md) · [下一章：把模块拆到不同文件中 →](ch07-05-separating-modules-into-different-files.md)

