# 把模块拆到不同文件中

到目前为止,一个模块始终对应着恰好一个文件。真实程序很少一直这么小。一旦一
个模块自己的源码需要分散到多个文件里,scpp 仍然只保留一个主接口单元,再让这
个模块的其余文件以 partition 的形式附着到它上面。

在今天普通的 `scpp build` project build 里,把一个模块拆成多个文件的受支持
方式,就是:一个主接口单元,再加上一个或多个 partition。

下面的每个例子都放在同一个 package 里。

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

## `export import :part;` 会重新导出一个 interface partition

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :trig;

namespace mathlib {
    export int sum_of_squares(int a, int b) {
        return a * a + b * b;
    }
}
```

`src/trig.scpp`:

```cpp
export module mathlib:trig;

namespace mathlib {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::sum_of_squares(3, 4));
    std::println("{}", mathlib::sin_deg(30));
    return 0;
}
```

输出:

```text
25
30
```

`mathlib.scpp` 依然是这个模块的主接口单元:它是那个写着 `export module
mathlib;` 的文件。`trig.scpp` 也属于同一个模块,但它用
`export module mathlib:trig;` 给这个模块命名了一个 partition。主接口单元
再用 `export import :trig;` 把它重新导出,所以模块外部的文件依然只写
`import mathlib;`。模块外部不会直接 import `:trig`。

## partition 名字不会变成路径里的另一个 `::` 段

上面的 partition 叫 `:trig`,但这并不会让 `trig` 变成任何导出条目路径的一部
分。

`src/main.scpp`:

```cpp
import mathlib;

int main() {
    return mathlib::trig::sin_deg(30);
}
```

编译器输出:

```text
src/main.scpp:4:12: error: call to unknown function 'mathlib::trig::sin_deg'
 4 |     return mathlib::trig::sin_deg(30);
   |            ^
```

`sin_deg` 是从 `namespace mathlib` 里导出的,所以它的路径是
`mathlib::sin_deg`。partition 名字 `trig` 只是帮助组织这个模块自己的源文件,
不会新建一个 namespace 段,也不会改动任何限定名。跟上一节一样,路径依然只来
自声明本身的 namespace 嵌套。

## `import :part;` 会把 partition 留在模块内部

partition 也可以只为了模块内部使用而被 import。

`src/mathlib.scpp`:

```cpp
export module mathlib;

import :detail;

namespace mathlib {
    export int doubled_sum(int a, int b) {
        return double_it(a + b);
    }
}
```

`src/detail.scpp`:

```cpp
module mathlib:detail;

namespace mathlib {
    int double_it(int x) {
        return x * 2;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib;

int main() {
    std::println("{}", mathlib::doubled_sum(3, 4));
    return 0;
}
```

输出:

```text
14
```

这里的 `detail.scpp` 是一个 implementation partition:
`module mathlib:detail;` 这行自己的模块声明上没有 `export`。主接口单元用
普通的 `import :detail;` 在内部接上它,这就足够让 `doubled_sum` 调用
`double_it` 了。

但 importer 依然不能直接碰到 `double_it`:

```cpp
import std;
import mathlib;

int main() {
    return mathlib::double_it(5);
}
```

编译器输出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::double_it'
 5 |     return mathlib::double_it(5);
   |            ^
```

普通的 `import :detail;` 只是在把另一个文件接进同一个模块自己的实现里。它不
会让这个 partition 变成模块公开接口的一部分。

## 只有模块自己的文件才能 import 一个 partition

模块外部的文件仍然不能在 `import` 后面写 partition 名字。

`src/main.scpp`:

```cpp
import mathlib:trig;

int main() {
    return 0;
}
```

编译器输出:

```text
src/main.scpp:1:15: error: expected ';' but found ':'
 1 | import mathlib:trig;
   |               ^
```

对模块外部来说,`import` 依然是在命名一个完整模块,这一点跟上一节完全一样。
`:trig` 这种写法,只给那些已经属于 `mathlib`、还要再 import 这个模块另一部
分的文件使用。外部代码 import 的仍然是整个模块,拿到的是主接口单元选择重新
导出的那些内容。

## `export import` 只能用于 interface partition

implementation partition 从构造上就是内部细节,所以试图重新导出它会被拒绝。

`src/mathlib.scpp`:

```cpp
export module mathlib;

export import :detail;
```

编译器输出:

```text
src/mathlib.scpp:3:8: error: cannot 'export import' partition 'mathlib:detail': it is an implementation partition ('module ...;' with no 'export' on its own module declaration), so it can never export anything to the outside (ch11 §11.4)
 3 | export import :detail;
   |        ^
```

只有 interface partition -- 也就是那些写着 `export module name:part;` 的
文件 -- 才能被重新导出。implementation partition 可以贡献只在模块内部使用
的代码,但它永远不能进入模块的导出面。

## 一个模块、一个主接口、多个文件

- 依然只有一个文件声明 `export module mathlib;` -- 这就是主接口单元;
- 额外文件通过 `module mathlib:part;` 或 `export module mathlib:part;`
  加入同一个模块;
- 模块外部文件依然写 `import mathlib;`,不会直接写 partition 名字;
- partition 名字是组织源文件用的,不是限定路径的一部分;
- `export import :part;` 会重新导出一个 interface partition,而普通的
  `import :part;` 则把 partition 留在模块内部。

有了 partition,一个模块就可以长到跨越多个文件,而不需要推翻前几节已经讲过
的任何规则:可见性仍然由 `export` 决定,路径仍然由 namespace 决定,`import`
仍然带来的是整个模块,而不是单独某个条目。

---

[← 上一章：使用 `import` 与限定名](ch07-04-using-import-and-qualified-names.md) · [目录](README.md)
