# 包与项目清单

到目前为止，你写的每个 scpp 程序都只放在一个文件里，或者就是你在 [第一个
project build](ch01-03-hello-project-builds.md) 里构建过的那种小型的、基于清单
的项目。这一章要往前走一步：一个真正的 scpp 项目是怎么组织的，一个项目怎样产出
不止一个二进制程序，以及这些二进制程序之间又怎样共享代码。

这一章接下来会反复用到两个词，它们说的不是同一件事：

- **包**是一个根目录下带有 `scpp.toml` 清单的目录，是 `scpp build` 操作的基本
  单位。
- **模块**是由 `export module` / `module` 引入、由 `import` 使用的编译单元；编
  译器在检查“从哪里能看到什么”时，正是以模块为单位来推理的。

一个包的清单可以声明任意数量的二进制目标，也可以声明任意数量的库。这些库和这些
二进制程序之间要靠模块才能真正共享声明。这一节始终停留在包这一层；下一节会深入
到模块内部。

对于下面的每一个项目，先创建好展示的文件，然后在该项目自己的目录里运行：

```sh
scpp build
```

二进制程序会落在 `.scpp/build/<target triple>/dev/<package name>/<binary
name>` 下面，和 [第一个 project build](ch01-03-hello-project-builds.md) 里完全
一样。

## 一个包需要一份清单和至少一个目标

光有 `manifest-version = 1`，再加上一个带 `name` 和 `version` 的 `[package]`
表，还不够。清单还需要至少一个构建目标：一个 `[[bin]]` 表、一个 `[[lib]]` 表，
或者两者都有。

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"
```

```sh
scpp build
```

编译器输出：

```text
error: manifest must declare at least one [[lib]] or [[bin]] target
```

这一节接下来会在同一份清单上逐个添加目标。

## `sources` 是你自己写的 glob 模式，不是固定文件名

一个 `[[bin]]` 表需要 `name` 和 `sources` 两个字段。`name` 同时决定了二进制程
序自己的文件名，以及之后 `--bin` 用来选中它时所用的名字。`sources` 是一组 glob
模式，会针对包自己的目录树展开——`*` 只在一层目录内匹配，`**` 可以跨目录匹配。
scpp 不会为二进制程序的入口保留任何固定文件名；具体的目录布局完全由你通过
`sources` 自己决定。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["*.scpp"]
```

`greeter.scpp`：

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
```

输出：

```text
Hello, scpp!
```

这里 `sources = ["*.scpp"]` 匹配包根目录下所有直接存在的 `.scpp` 文件——目前还
没有 `src/` 目录，也不是必须要有。

## 一个包可以构建出不止一个二进制程序

一份清单可以声明任意数量的 `[[bin]]` 表。每一个都会根据自己指定的源文件独立编
译、链接，互不影响。当项目的文件不止一个之后，把源文件放到 `src/` 下面，可以让
每个目标的 glob 模式精确指向自己名下的那些文件。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greeter.scpp`：

```cpp
import std;

int main() {
    std::println("Hello, scpp!");
    return 0;
}
```

`src/shout.scpp`：

```cpp
import std;

int main() {
    std::println("HELLO, SCPP!");
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

输出：

```text
Hello, scpp!
HELLO, SCPP!
```

直接运行 `scpp build` 会构建出两个二进制程序。如果只想构建其中一个，需要显式点
名：

```sh
scpp build --bin shout
./.scpp/build/*/dev/greeter/shout
```

输出：

```text
HELLO, SCPP!
```

`[[bin]]` 的名字在同一个包里也必须唯一。如果两个 `[[bin]]` 表都叫 `"greeter"`，
会在两者都还没编译之前就被拒绝：

```text
error: duplicate [[bin]] target name 'greeter'
```

## 库目标让二进制程序之间自动共享代码

`[[lib]]` 表和 `[[bin]]` 很像——同样是 `name` 和 `sources` 两个字段——但它编译出
来的是一个模块，而不是链接出一个可执行文件。在这个模块的源文件里，`export
module greetings;` 为模块命名，与之对应的 `namespace greetings { ... }` 则标出
哪些声明会被 `export` 暴露给导入方。

同一个包里的每个 `[[bin]]` 目标都可以直接用名字 `import` 这个模块，不需要任何
额外的参数：`scpp build` 在同一次运行里早些时候就已经把它编译好了，清单也记录
了它的接口和归档文件放在哪里，供后续构建使用。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

`src/greetings.scpp`：

```cpp
export module greetings;

namespace greetings {
    export const char* phrase(bool shout) {
        return shout ? "HELLO, SCPP!" : "Hello, scpp!";
    }
}
```

`src/greeter.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(false));
    return 0;
}
```

`src/shout.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

输出：

```text
Hello, scpp!
HELLO, SCPP!
```

输出和之前一样，但现在 `greeter` 和 `shout` 共享同一份实现，而不是在每个文件里
各自重复一遍这句话。构建过程还会在两个二进制程序旁边，留下库自己编译出的产物：

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
```

`--lib` 只构建库本身，不会链接任何一个二进制程序：

```sh
scpp build --lib
```

## 一个包也可以构建出不止一个库

一份清单可以声明任意数量的 `[[lib]]` 表，就像它可以声明任意数量的 `[[bin]]` 表
一样。包里的每个 `[[bin]]` 目标都可以 `import` 其中任意一个，不只是它恰好需要
的那一个——上面只用了一个 `[[lib]]` 表，并没有什么特殊之处。

`scpp.toml`：

```toml
manifest-version = 1

[package]
name = "greeter"
version = "0.1.0"

[[lib]]
name = "greetings"
sources = ["src/greetings.scpp"]

[[lib]]
name = "farewells"
sources = ["src/farewells.scpp"]

[[bin]]
name = "greeter"
sources = ["src/greeter.scpp"]

[[bin]]
name = "shout"
sources = ["src/shout.scpp"]
```

这里新增了第二个库 `farewells`，和已有的 `greetings` 放在一起。

`src/farewells.scpp`：

```cpp
export module farewells;

namespace farewells {
    export const char* phrase() {
        return "Goodbye, scpp!";
    }
}
```

`greeter` 现在导入了两个库——不需要任何清单选项来开启这一点，只要 `import` 就够
了。

`src/greeter.scpp`：

```cpp
import std;
import greetings;
import farewells;

int main() {
    std::println("{}", greetings::phrase(false));
    std::println("{}", farewells::phrase());
    return 0;
}
```

`shout` 没有变化，仍然只导入 `greetings`——一个 `[[bin]]` 目标可以自由导入包里
`[[lib]]` 目标的任意子集，不必导入全部。

`src/shout.scpp`：

```cpp
import std;
import greetings;

int main() {
    std::println("{}", greetings::phrase(true));
    return 0;
}
```

```sh
scpp build
./.scpp/build/*/dev/greeter/greeter
./.scpp/build/*/dev/greeter/shout
```

输出：

```text
Hello, scpp!
Goodbye, scpp!
HELLO, SCPP!
```

构建过程现在会把两个库各自的产物并排留下：

```text
.scpp/build/*/dev/greeter/modules/greetings.scppm
.scpp/build/*/dev/greeter/modules/farewells.scppm
.scpp/build/*/dev/greeter/archives/libgreetings.scppa
.scpp/build/*/dev/greeter/archives/libfarewells.scppa
```

单独使用 `--lib` 仍然会构建每一个库目标，不链接任何二进制程序，和只有一个库时
一样。要在多个库中只构建某一个，就像 `--bin` 选择某个二进制程序一样，点名它即
可：

```sh
scpp build --lib farewells
```

这样只会构建 `farewells`；`greetings` 和两个二进制程序都不受影响。

`[[lib]]` 的名字在同一个包里也必须唯一，和 `[[bin]]` 的名字一样。如果两个
`[[lib]]` 表都叫 `"greetings"`，也会被同样地拒绝：

```text
error: duplicate [[lib]] target name 'greetings'
```

## 目前为止的清单规则

- 包是一个根目录下有 `scpp.toml` 的目录；
- 清单需要 `manifest-version = 1`、一个 `[package]` 表，以及至少一个 `[[bin]]`
  或 `[[lib]]` 表；
- `sources` 是你自己写的一组 glob 模式，不是固定的文件名；
- 一个包可以构建任意数量的二进制程序，用 `--bin <name>` 单独选择要构建哪一个；
- 一个包可以构建任意数量的库，用 `--lib <name>` 单独选择要构建哪一个，或者单独
  用 `--lib` 一次构建全部；
- 包里的每个 `[[bin]]` 目标都会自动看到这个包所有的 `[[lib]]` 模块，不需要任何
  `--import` 参数。

包讲的是构建层面的故事。下一节会转到语言层面：模块如何用命名空间，在这些文件内
部和之间控制作用域与可见性。

---

[← 上一章：如何把“信任”局部化到真实程序里](ch06-03-localizing-trust-in-real-programs.md) · [目录](README.md)
