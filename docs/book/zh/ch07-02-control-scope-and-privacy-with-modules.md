# 用模块控制作用域与可见性

上一节始终停留在包这一层：清单里的 `[[bin]]` 和 `[[lib]]` 表，以及 `scpp
build` 如何让同一个包里的二进制程序共享另一个目标编译出的模块。这一节要往下深
入到语言本身：一旦某个二进制程序可以 `import` 一个模块，它到底能拿到些什么？

简短的答案是：拿到的东西比整个文件要少得多。一个模块自己的源码里，尽是些永远
不会离开这个文件的普通声明。只有同时满足下面两个条件，一个声明才会对导入方可
见：

- 它被标记为 `export`；
- 它被声明在一个和模块自身名字相匹配的命名空间里。

只要有一个条件没满足，这个声明就仍然是私有的——在模块自己的文件内部可以正常使
用，在其他任何地方都不可见。

下面的每一个例子都放在同一个包里。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "mathlib-app"
version = "0.1.0"

[[bin]]
name = "app"
sources = ["src/*.scpp"]
```

这里只用一个带 glob `sources` 模式的 `[[bin]]` 目标就够了：正如[包与项目清
单](ch07-01-packages-and-project-manifests.md)一节讲过的，一个目标的
`sources` 可以指向不止一个文件，而这些文件里的任意一个都可以是模块，被同一个
目标里的另一个文件导入。这一节接下来不需要再改动这份清单——只需要改动
`src/` 目录下的 `.scpp` 文件。用下面的命令构建并运行每一个版本：

```sh
scpp build
./.scpp/build/*/dev/mathlib-app/app
```

## 只有被导出的声明才能在模块外部可见

`src/mathlib.scpp`：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

输出：

```text
25
```

`square` 是一个普通函数：没有 `export`，也没有外层命名空间。`sum_of_squares`
则在 `namespace mathlib` 内部被 `export`，与模块自身的名字相匹配，所以
`main.scpp` 才能以 `mathlib::sum_of_squares` 的形式访问它。`square` 本身从未
跨出模块边界——`sum_of_squares` 仍然可以在内部直接调用它，因为这条规则只关心
*导入方*能看到什么，并不关心模块自己的代码内部能用什么。

直接从导入方这边访问 `square`，可以验证这一点：

```cpp
import std;
import mathlib;

int main() {
    return square(5);
}
```

编译器输出：

```text
src/main.scpp:5:12: error: call to unknown function 'square'
 5 |     return square(5);
   |            ^
```

从 `main.scpp` 的角度看，`square` 根本就没有被声明过。这并不是一个访问控制上
的错误——这个名字压根就没有进入这个文件的作用域，就好像它从来没被写出来过一
样。

## 导出的声明必须位于和模块名匹配的命名空间里

上面单靠 `export` 还不够。`sum_of_squares` 还必须被声明在 `namespace mathlib
{ ... }` 内部——一个和模块自身名字匹配的命名空间。`export` 和“位于所要求的命
名空间内”是两个相互独立、缺一不可的条件。

（模块自己的名字可以有好几段用点分隔的部分，比如 `mathlib.trig`；每一段都会一
一对应到一段用 `::` 分隔的命名空间。这一节里的每个模块都只用单段名字，所以它
所要求的命名空间就是这一个名字本身——下一节会讲多段模块名，以及它们对应出来的
路径。）

完全不写命名空间会被拒绝：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

export int sum_of_squares(int a, int b) {
    return square(a) + square(b);
}
```

编译器输出：

```text
src/mathlib.scpp:7:8: error: exported function 'sum_of_squares' must be declared inside a namespace -- ch11 §11.5
 7 | export int sum_of_squares(int a, int b) {
   |        ^
```

在无关的命名空间里导出，同样会被拒绝：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace geometry {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

编译器输出：

```text
src/mathlib.scpp:8:12: error: exported function 'geometry::sum_of_squares' must be declared in namespace matching module 'mathlib' -- ch11 §11.5
 8 |     export int sum_of_squares(int a, int b) {
   |            ^
```

`geometry` 和这个模块自己的名字 `mathlib` 毫无关系，所以它会被拒绝，原因与上
面缺少命名空间的情况完全一样。

## 比模块自身名字嵌套得更深也没问题

命名空间的要求是一种前缀匹配，而不是精确匹配。一个名为 `mathlib` 的模块，要求
它导出的声明位于 `namespace mathlib` 内部，或者位于比它嵌套得更深的任何命名空
间里——比如 `mathlib::trig`。

`src/mathlib.scpp`：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

输出：

```text
25
30
```

`mathlib::trig` 以 `mathlib` 开头，所以满足了这个要求，导入方也能通过完整的
嵌套名字 `mathlib::trig::sin_deg` 访问到 `sin_deg`。一个模块可以自由使用嵌套
命名空间来组织自己的内部结构；这条规则只关心每一个导出声明的命名空间是否以模
块自身的名字开头。

## 未导出的声明可以位于任何命名空间，或者根本不在任何命名空间里

前两节里的命名空间规则，只约束*被导出*的声明。一个从未被导出的声明，可以位于
任意一个和模块名毫不相干的命名空间里，也可以完全不在任何命名空间里——就像最开
始那个例子里的 `square` 一样。

`src/mathlib.scpp`：

```cpp
export module mathlib;

namespace detail {
    int square(int x) {
        return x * x;
    }
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return detail::square(a) + detail::square(b);
    }
}
```

`src/main.scpp`：

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    return 0;
}
```

输出：

```text
25
```

`detail` 和 `mathlib` 没有任何关系，但因为里面的 `square` 从未被 `export`，
这种不匹配也就无关紧要了。`sum_of_squares` 仍然可以调用 `detail::square`，因
为这次调用发生在模块自己的文件内部。导入方却做不到：

```cpp
import std;
import mathlib;

int main() {
    return detail::square(5);
}
```

编译器输出：

```text
src/main.scpp:5:12: error: call to unknown function 'detail::square'
 5 |     return detail::square(5);
   |            ^
```

即便加上 `detail::` 限定也无济于事——`main.scpp` 从一开始就没有把
`detail::square` 引入自己的作用域，因为它从未被导出过。

## 普通的 `import` 是私有且不可传递的

到目前为止，每一个导入方都是直接跟声明所需内容的那个模块打交道。真正的项目往
往会把模块串联起来：一个模块导入另一个模块来构建自己的功能，再把自己的一部分
功能暴露给第三个文件。第三个文件到底能看到什么，完全取决于中间这个模块是怎么
导入它自己的依赖的。

再加入第二个模块 `stats`，它以普通方式导入 `mathlib`，并在内部使用它：

`src/mathlib.scpp`（恢复成第一节里那个最简单的版本）：

```cpp
export module mathlib;

int square(int x) {
    return x * x;
}

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return square(a) + square(b);
    }
}
```

`src/stats.scpp`：

```cpp
export module stats;

import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`，只导入 `stats`：

```cpp
import std;
import stats;

int main() {
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

输出：

```text
50
```

这样是可以的：`main.scpp` 完全没有提到 `mathlib`，而 `sum_of_squares_twice`
也正确地在 `namespace stats` 内部被 `export`。但 `stats.scpp` 自己的
`import mathlib;` 是一次普通导入，而普通导入只对写下它的那个文件私有。它只让
`mathlib` 的导出内容在 `stats.scpp` 里可见，仅此而已：

```cpp
import std;
import stats;

int main() {
    return mathlib::sum_of_squares(3, 4);
}
```

编译器输出：

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::sum_of_squares'
 5 |     return mathlib::sum_of_squares(3, 4);
   |            ^
```

`main.scpp` 从未导入过 `mathlib`，而 `stats` 自己那句普通的 `import
mathlib;` 也不会把它转发出去。只有 `stats` 自己导出的名字——这里就是
`sum_of_squares_twice`——才会进入 `main.scpp`。

## `export import` 会传递式地重新导出

把普通的 `import` 换成 `export import`，情况就不一样了：它会把被导入模块导出
的一切都重新导出，并且是可传递的——传递给任何转而导入当前模块的文件。

`src/stats.scpp`，改成重新导出：

```cpp
export module stats;

export import mathlib;

namespace stats {
    export int sum_of_squares_twice(int a, int b) {
        return mathlib::sum_of_squares(a, b) * 2;
    }
}
```

`src/main.scpp`，依然只导入 `stats`：

```cpp
import std;
import stats;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", stats::sum_of_squares_twice(3, 4));
    return 0;
}
```

输出：

```text
25
50
```

关键的改动只在 `stats.scpp` 内部：`import mathlib;` 变成了 `export import
mathlib;`。`main.scpp` 自己的导入列表没有变化——仍然只有一句 `import
stats;`——但它现在可以直接以 `mathlib` 自己的名字调用
`mathlib::sum_of_squares` 了。重新导出并不会重命名或者包装它转发的内容，它只
是扩大了能访问到这些内容的范围。

## 目前为止的私有性与可见性规则

- 一个声明默认对自己所在的模块私有，除非有什么东西把它导出；
- `export` 只有在声明位于一个和模块自身名字匹配的命名空间内部时才会生效——嵌
  套得更深没问题，位于其他任何地方都会被拒绝；
- 一个从未被导出的声明可以位于任意命名空间，或者根本不在任何命名空间里，因为
  命名空间规则根本不适用于它；
- 一句普通的 `import name;` 只对写下它的文件私有：它让 `name` 的导出内容在
  这个文件里可见，但不会把这些内容转发给转而导入这个文件的其他文件；
- `export import name;` 会把 `name` 自己的导出内容以它们各自原本的限定名字，
  传递式地重新导出给每一个更外层的导入方。

这一节里用到的每一个名字，都是一次性完整写出的、用点或 `::` 限定的路径。下一
节会更仔细地研究这些路径本身：一个模块自己的点分名字是如何映射到它的命名空间
树上的，以及在这棵树里从一个位置引用另一个位置的导出内容时，应该遵循的规则。

---

[← 上一章：包与项目清单](ch07-01-packages-and-project-manifests.md) · [目录](README.md)
