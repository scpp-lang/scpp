# 定义与实例化 `struct` / `class`

`struct` 或 `class` 能把一组相关字段收拢到同一个名字下面。这样一来，代码就不
用到处传好几个分离的值，而是可以先定义一个“这些字段天然属于一起”的类型。

在 scpp 里，`struct` 和 `class` **不是**可互换的两种写法：

- `struct` 是 plain-data、非多态的那一种。它仍然可以有 `public:` /
  `private:` 区段、构造函数，以及普通的非虚成员函数。
- `class` 则是为继承与多态准备的那一种形式。它也可以持有 `std::string` 这样
  的非平凡 class 类型字段。

这种划分是刻意为之的。选择 `struct`，等于在说“这个类型不会进入继承、
interface 与虚调用的世界”。选择 `class`，则是从一开始就显式进入那个世界：每
一个 `class` 都必须声明 `virtual` 析构函数，即使它什么都不做。这样一来，以后
再加入虚函数或 interface base 时，就不会悄悄改变这个类型的形状；同时，scpp
也避免了 C++ 里那种“把类当基类用了，却忘了写 virtual 析构函数”的经典陷阱。

后面的章节会详细介绍继承和 interface。眼下先记住一条分界线：只有 `class` 能
参与那套系统。`struct` 不能声明虚成员，不能写 base-clause，也不能标成
`[[scpp::interface]]`；反过来，一个用 `struct` 声明出来的类型，后面也不能被某
个 `class` 拿去当 base。

下面每个可执行示例都可以保存成 `records.scpp`，然后这样构建并运行：

```sh
scpp records.scpp -o records
./records
```

对于那些本来就应该被编译器拒绝的示例，如果你希望得到与书里逐字一致的诊断输
出，请把文件保存成诊断块里显示的那个描述性文件名。

## 定义一个带命名字段的基础 `struct`

只有字段的 `struct`，是把相关数据放在一起的最简单方式。

```cpp
import std;

struct User {
    int id{};
    const char* name{""};
};

int main() {
    User user{};
    user.id = 7;
    user.name = "Ada";
    std::println("{} {}", user.id, user.name);
    return 0;
}
```

输出：

```text
7 Ada
```

`User user{};` 会创建一个 `User` 值，并先对字段做默认初始化。之后，字段就可以
用普通的点语法来读写。

在当前 scpp 里，像 `User user{7, "Ada"};` 这样的带参数花括号，并不会自动把值
填进 public 字段里。如果你想在构造时就传参数，就要定义构造函数。

## `struct` 仍然可以隐藏字段并定义行为

在 scpp 里，如果你只是想隐藏数据或定义构造函数，**并不需要**因此切到 `class`。
`struct` 仍然可以有 `private:` 区段、default constructor、parameterized
constructor，以及普通的非虚成员函数。

```cpp
import std;

struct Size {
private:
    int width{};
    int height{};

public:
    Size() {
        return;
    }

    Size(int initial_width, int initial_height)
        : width{initial_width}, height{initial_height} {
        return;
    }

    void grow_width(int delta) {
        this->width = this->width + delta;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Size empty{};
    Size window{3, 4};
    window.grow_width(1);
    std::println("{} {}", empty.area(), window.area());
    return 0;
}
```

输出：

```text
0 16
```

这里的 `Size` 仍然是一个 `struct`，尽管它把字段藏了起来，还围绕这些字段定义了
行为。我们会在第 5.3 节再回到方法语法；现在更重要的点是：只要一个类型应该保
持“非虚、非继承”的数据形态，`struct` 仍然就是那个普通而正统的工具。

## 单参数构造函数也可以在调用点触发转换

单参数构造函数还可以充当 converting constructor。这意味着：如果一个函数按值接
收该类型，那么调用点也可以直接传入那个构造参数。

```cpp
import std;

struct Meters {
    int value{};

public:
    Meters(int initial_value) : value{initial_value} {
        return;
    }
};

int read(Meters meters) {
    return meters.value;
}

int main() {
    Meters direct{8};
    std::println("{} {}", read(5), direct.value);
    return 0;
}
```

输出：

```text
5 8
```

这仍然是普通的构造。`read(5)` 之所以可行，是因为 scpp 会先从 `5` 构造一个临时
的 `Meters`，再用它来满足这个按值参数。

## `struct` 的字段必须保持 plain data

scpp 里的 `struct` 仍然只接受 plain-data 字段类型。如果某一个字段需要
`std::string` 这样的 class 行为，那么外围这个类型就必须改成 `class`。

```cpp
import std;

struct Bad {
    std::string name{"hi"};
};

int main() {
    Bad value{};
    return 0;
}
```

编译器输出：

```text
struct_string_field_fail.scpp: error: struct 'Bad' field 'name': a class type 'std::string' cannot be a struct field; use class instead (only scalars, pointers, trivial structs/unions, and fixed-size arrays of trivial types are allowed here; see spec ch04)
```

这也是它和普通 C++ 的一大区别：在普通 C++ 里，`struct` 和 `class` 往往主要只差
默认访问级别；但在 scpp 里，它们承诺的能力边界本身就不同。

## 定义并实例化一个 `class`

在使用点上，`class` 看起来仍然很熟悉：你同样会定义字段、用花括号构造一个值，
并且用点语法访问 public 字段。

```cpp
import std;

class DisplayName {
public:
    std::string text;

    DisplayName(const char* initial_text) : text{initial_text} {
        return;
    }

    virtual ~DisplayName() {
        return;
    }
};

int main() {
    DisplayName name{"scpp"};
    std::println("{}", name.text.c_str());
    return 0;
}
```

输出：

```text
scpp
```

表面语法很简单，但这里的设计选择已经和 `struct` 不一样了。这个类型现在可以持
有 `std::string`，而且因为它是 `class`，它也进入了语言里“以后可以有一个普通
base class，再加任意多个 interface base”的那一侧。

## 每个 `class` 都必须显式声明 virtual 析构函数

如果你省掉这个析构函数，那么即使这个类没有别的虚成员，程序也仍然是 ill-formed
的。

```cpp
class Account {
public:
    Account() {
        return;
    }
};

int main() {
    Account account{};
    return 0;
}
```

编译器输出：

```text
class_without_virtual_dtor_fail.scpp: error: class 'Account' must declare an explicit virtual destructor (spec §11.5(1))
```

所以在 scpp 里，选择 `class` 并不只是风格偏好。它是语言里专门留给继承与多态
的那种形式，而析构函数规则就是让这个选择从一开始就明确、稳定的一部分。

## 默认花括号初始化仍然需要 default constructor

`Type value{};` 的意思是“用零个构造参数来构造一个值”。如果一个类型只声明了带
参数构造函数，那么这种初始化会被正常地以“构造函数选择失败”的方式拒绝。

```cpp
struct CtorOnly {
    int value;

public:
    CtorOnly(int x) : value{x} {
        return;
    }
};

int main() {
    CtorOnly value{};
    return 0;
}
```

编译器输出：

```text
struct_default_ctor_fail.scpp:11:5: error: type 'CtorOnly' has no default constructor; no constructor of 'CtorOnly' matches 0 arguments
 11 |     CtorOnly value{};
    |     ^
```

同样的规则也适用于 `class`。如果你希望 `Type value{};` 成立，那么这个类型就真
的必须有 default constructor。

## `struct` 不能声明虚成员

反过来，`struct` 这一侧的限制也同样重要：`struct` 永远不是虚的。

```cpp
struct Plain {
    virtual void f() {
        return;
    }
};

int main() {
    Plain value{};
    return 0;
}
```

编译器输出：

```text
struct_virtual_member_fail.scpp:2:5: error: a declaration introduced by 'struct' shall not declare a virtual member function or virtual destructor (spec §11.1(2.3))
 2 |     virtual void f() {
   |     ^
```

如果一个类型需要 virtual 行为，它就必须是 `class`。

## `struct` 不能继承

同样地，在 scpp 里，`struct` 也不是用来继承的那种形式。

```cpp
class Base {
public:
    Base() {
        return;
    }

    virtual ~Base() {
        return;
    }
};

struct Derived : public Base {
    Derived() {
        return;
    }
};

int main() {
    Derived value{};
    return 0;
}
```

编译器输出：

```text
struct_inherit_fail.scpp:12:16: error: a declaration introduced by 'struct' shall not have a base-clause (spec §11.1(2.1))
 12 | struct Derived : public Base {
    |                ^
```

同一条边界在线的另一侧也成立：后面某个 `class` 也不能把 `struct` 当作 base，
而且 `struct` 也不能标成 `[[scpp::interface]]`。如果某个类型将来可能参与继承或
interface，就应该从一开始把它定义成 `class`。

## `struct` 与 `class` 规则小结

到这里，工作规则可以总结成：

- 当一个类型应该保持 plain-data、非虚、非继承时，用 `struct` 来组织相关数据；
- `struct` 仍然可以有 `public:` / `private:` 区段、构造函数，以及普通的非虚成
  员函数；
- 单参数构造函数可以在调用点充当 converting constructor；
- 无论是 `struct` 还是 `class`，字段都用点语法访问；
- 如果你想写 `Type value{};`，这个类型就真的必须有 default constructor；
- `struct` 不能持有 `std::string` 这样的 ownership-tracked 字段，不能声明虚成
  员，不能写 base-clause，也不能成为 interface；
- 每一个 `class` 都必须显式声明一个 `virtual` 析构函数；
- 在 scpp 里，只有 `class` 才会参与继承、虚调用，以及 interface 实现。

下一节会围绕一个“受检查的 `class`”搭一个小示例程序。

---

[← 上一章：`std::span` 与其它非拥有视图](ch04-03-std-span-and-other-non-owning-views.md) · [目录](README.md)
