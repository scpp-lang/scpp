# 在模块树中引用条目的路径

上一节里,每一个条目都是通过路径来访问的:`mathlib::sum_of_squares`、
`mathlib::trig::sin_deg`、`stats::sum_of_squares_twice`。每一次都是在调用的地
方完整地写出来。这一节要看看路径到底是什么,模块自己的点分名字如何决定路径的
形状,以及 scpp 在跳过其中任何一部分这件事上,到底能提供多少简写。

一个路径不过是一个条目的命名空间嵌套层级,用 `::` 连接起来,最后加上它自己的
名字。它和声明这个条目的文件是哪一个毫无关系,而且它和模块自己的点分导入名字
是两回事——下面第一节会说明为什么这两者用了不同的分隔符。

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

## 模块的点分名字,按段对应到路径

[用模块控制作用域与可见性](ch07-02-control-scope-and-privacy-with-modules.md)
曾经顺带提到,模块自己的名字可以有好几段用点分隔的部分,比如
`mathlib.trig`;每一段都会一一对应到一段用 `::` 分隔的命名空间。到目前为止的
每个模块都只用单段名字,这一点从来没有真正用到过。用一个真正的两段式模块名
字,可以把它坐实。

`src/trig.scpp`:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    export int sin_deg(int x) {
        return x;
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(30));
    return 0;
}
```

输出:

```text
30
```

`import mathlib.trig;` 用点来给模块命名,和模块自己给自己命名的方式一样。访
问它导出的内容仍然用 `::`,和之前完全一样——`mathlib::trig::sin_deg`。模块自
己的两段用点分隔的部分,`mathlib` 和 `trig`,变成了两段必须匹配的 `::` 分隔命
名空间,`mathlib` 和 `trig`。这仍然是上一节那条命名空间匹配规则,只是模块自己
的名字现在有不止一段需要匹配。

这条规则依然完全成立:比模块自身名字少一段的命名空间会被拒绝,就像之前完全没
有命名空间会被拒绝一样。

```cpp
export module mathlib.trig;

namespace mathlib {
    export int sin_deg(int x) {
        return x;
    }
}
```

编译器输出:

```text
src/trig.scpp:4:12: error: exported function 'mathlib::sin_deg' must be declared in namespace matching module 'mathlib.trig' -- ch11 §11.5
 4 |     export int sin_deg(int x) {
   |            ^
```

`namespace mathlib` 只提供了两段里的第一段,所以它会被当成任何一个不匹配的命
名空间来处理:和无关命名空间、或者完全没有命名空间,是同一种错误。

## 只有声明在同一个命名空间里的名字才能省略路径

一个命名空间仍然可以比模块自身名字嵌套得更深——这部分规则和上一节完全一样,
没有变化。这里真正新出现的,是一次不带路径的调用到底能访问到什么,一旦命名空
间开始嵌套起来。

`src/trig.scpp`,加上一个辅助函数和一个嵌套更深的命名空间:

```cpp
export module mathlib.trig;

namespace mathlib::trig {
    int double_it(int x) {
        return x * 2;
    }

    export int sin_deg(int x) {
        return double_it(x);
    }
}

namespace mathlib::trig::deg {
    export int right_angle() {
        return mathlib::trig::double_it(45);
    }
}
```

`src/main.scpp`:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", mathlib::trig::sin_deg(400));
    std::println("{}", mathlib::trig::deg::right_angle());
    return 0;
}
```

输出:

```text
800
90
```

`sin_deg` 完全不带路径地调用 `double_it`,这样是可以的,因为两者都直接声明在
同一个 `namespace mathlib::trig { ... }` 块里。`right_angle` 声明在更深一层的
`mathlib::trig::deg` 里,它通过完整路径 `mathlib::trig::double_it` 调用同一个
`double_it`——即使 `mathlib::trig` 就是它自己直接外层的命名空间。去掉这个路
径,直接在 `mathlib::trig::deg` 里面裸调用 `double_it`,是不行的:

```cpp
namespace mathlib::trig::deg {
    export int right_angle() {
        return double_it(45);
    }
}
```

编译器输出:

```text
src/trig.scpp:15:16: error: call to unknown function 'double_it'
 15 |         return double_it(45);
    |                ^
```

一次不带路径的调用,只能访问到直接声明在同一个命名空间里的名字(或者像之前每
个例子里的普通函数那样,根本不在任何命名空间里的名字)。它不会像查找嵌套代码
块里的变量那样,向外逐层攀爬命名空间。要跨到任何其他命名空间——哪怕只是直接包
住当前这个命名空间的那一层——都必须写出完整路径。

## 开头的 `::` 会从最外层作用域开始查找

一个路径也可以以 `::` 开头。这不会改变一个像 `mathlib::trig::sin_deg` 这样完
整写出的路径所访问到的内容——它访问到的仍然是同一个条目——但它能保证查找是从
最外层作用域开始的,而不会考虑调用处已经在作用域里的其他任何东西。这个区别只
有在调用处的裸名字本来就可能访问到别的东西时,才会显现出来。

```cpp
import std;

int count() {
    return 100;
}

int main() {
    int count = 7;
    std::println("{}", count);
    std::println("{}", ::count());
    return 0;
}
```

输出:

```text
7
100
```

两个 `count` 在 `main` 内部都是可见的:一个局部变量,还有一个同名的普通函
数——如果没有开头的 `::`,后者本来就会被局部变量遮蔽掉。裸写 `count` 访问到的
是局部变量。写 `::count()` 则会直接跳过它,访问到声明在最外层作用域的那个函
数。同样,开头的 `::` 放在一个完整路径前面,含义也一样——从最外层作用域开始,
然后严格按照写出来的路径往下找:

```cpp
import std;
import mathlib.trig;

int main() {
    std::println("{}", ::mathlib::trig::sin_deg(400));
    std::println("{}", ::mathlib::trig::deg::right_angle());
    return 0;
}
```

输出:

```text
800
90
```

这里 `main` 的调用处,作用域里没有任何东西可能和 `mathlib::trig::sin_deg` 或
者 `mathlib::trig::deg::right_angle` 混淆,所以开头的 `::` 对结果没有任何影
响。它对任何路径都是可用的,不只是那些真正需要它的路径。

## 路径依然无法访问从未被导出的内容

以上这些都没有重新打开上一节的私有性规则。一个和某个声明的命名空间嵌套完全
匹配的路径,如果这个声明从未被 `export`,依然会失败——单单把路径写对是不够
的。

```cpp
import std;
import mathlib.trig;

int main() {
    return mathlib::trig::double_it(5);
}
```

编译器输出:

```text
src/main.scpp:5:12: error: call to unknown function 'mathlib::trig::double_it'
 5 |     return mathlib::trig::double_it(5);
   |            ^
```

`double_it` 确实就位于 `mathlib::trig::double_it`——这正是上面 `trig.scpp` 内
部自己用来调用它的那个路径——但它从未被导出,所以没有任何路径能从
`main.scpp` 访问到它。路径只描述一个东西位于哪里;它能不能从模块外部沿着这条
路径被找到,依然完全取决于 `export`。

## 到目前为止的路径规则

- 一个路径就是一个条目的命名空间嵌套层级,用 `::` 连接起来,最后加上它自己的
  名字——和声明它的文件是哪一个无关;
- 模块自己的点分名字,按段对应到它导出内容必须位于的 `::` 分隔命名空间,不管
  它有多少段;
- 一次不带路径的调用,只能访问到声明在同一个命名空间里的名字,或者根本不在任
  何命名空间里的名字——访问任何其他命名空间,包括直接包住调用处的那一层,都
  需要完整路径;
- 开头的 `::` 会让一个路径从最外层作用域开始查找,先于调用处已经在作用域里的
  任何其他东西;
- 一个路径始终只能访问到位置正确并且被 `export` 过的声明——把路径写对本身,
  并不能跨越上一节里的那道私有性边界。

这一节里的每一个路径,依然是在每一个需要它的文件里手动完整写出来的。下一节回
到 `import` 本身,看看它和一个路径自己的限定名字,在实际使用中是如何配合的。

---

[← 上一章：用模块控制作用域与可见性](ch07-02-control-scope-and-privacy-with-modules.md) · [目录](README.md)
