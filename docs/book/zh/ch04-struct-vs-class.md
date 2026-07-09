# struct 与 class 的语义区分（固定内存布局 / ABI）

在普通 C++ 里，`struct` 和 `class` 几乎是同一种工具，只差一个默认访问控制。
在 scpp 里，它们表达的是一个更大的设计选择：**这个值只是“按字节解释的数据”，
还是“要进入所有权/借用检查器世界的对象”？**

一个很实用的经验法则是：

- 需要**固定布局、按值拷贝、面向 ABI** 的数据时，用 `struct`；
- 需要**所有权、生命周期、方法、不变量** 时，用 `class`。

这条分界线是 scpp 的核心特色之一：低层互操作依然明确可写，但普通代码默认
待在 Rust 风格的静态安全模型里。

## 4.1 `struct`：纯平凡数据（trivial aggregate）

scpp 的 `struct` 是语言里的“纯数据形状”。它适合那种语义完全来自位模式和字段
布局，而不是来自所有权、析构或借用关系的值。

`struct` 的字段必须全部是**平凡（trivial）**类型，并且这个要求递归成立：

- 标量：`bool`、整数、浮点、`char`；
- 裸指针 `T*`；
- 函数指针（包括 [§5.16](ch05-static-checks.md#516-函数指针function-pointers)
  里那种带 `[[scpp::unsafe]]` 资格的函数指针类型）；
- 其它同样满足规则的 `struct` / `union`；
- 平凡类型的定长数组。

反过来说，下面这些**不能**放进 `struct`：引用、`std::span`、
`std::unique_ptr`、`std::shared_ptr`、`std::vector`、`std::string`，以及任何
语义依赖“所有权/借用/生命周期追踪”的类型。只要需要这些语义，类型就必须
改成 `class`。

这条限制换来了三个非常重要的性质。

第一，没有显式初始化器的 `struct` 局部变量会按位全零初始化。第二，复制一个
`struct` 永远只是复制它的字节，不存在隐藏的所有权转移，也不存在需要更新的
借用状态。第三，正因为字段全是平凡类型，`struct` 天然适合拿去做 C ABI 边界。

下面这个最小例子展示了最直接的结果：复制一个 `struct` 会得到一份彼此独立的
数据副本。

```cpp
extern "C" int puts(const char* s);

struct PacketHeader {
    int size;
    int kind;
};

int main() {
    PacketHeader original;
    original.size = 64;
    original.kind = 1;

    PacketHeader copy = original;
    copy.size = 128;

    [[scpp::unsafe]] {
        if (original.size == 64 && copy.size == 128) {
            puts("struct copies by value");
        }
    }
    return 0;
}
```

输出：

```text
struct copies by value
```

`original` 和 `copy` 不会彼此别名。对 `struct` 来说，赋值不是借用，不是移动，
也不是用户自定义逻辑——就是一次普通的值拷贝。

## 4.2 `class`：拥有资源 / 参与检查的类型

scpp 的 `class` 则是安全检查器真正开始工作的地方。`class` 可以包含别的
`class`、引用、智能指针、`span`，以及任何语义依赖所有权、借用、析构的字段。

实际使用里，只要满足下面任意一点，就应该考虑 `class`：

- 这个值拥有某种资源；
- 构造函数 / 析构函数本身有意义；
- 你希望编译器追踪它的 move、borrow、lifetime；
- 这个类型暴露的是“行为”，不只是“布局”。

同时，scpp 在这里比 C++ **刻意更严格**：

- `class` 会进入 [§5](ch05-static-checks.md) 的所有权/借用检查；
- move 构造和 move 赋值**永远由编译器提供**，用户自己写会被拒绝；
- copy 相关操作可以自定义，但必须服从 [§5.1](ch05-static-checks.md)
  里的规则；
- `class` 局部变量也不再有那种“普通 C++ 未初始化存储”的概念；在构造函数或
  显式初始化覆盖之前，仍然服从 scpp 的统一零初始化规则。

最重要的观念变化是：在 scpp 里，`class` **不是**“带方法的 `struct`”。
`class` 是一种受检查的值：它的 API、移动语义、析构语义，本身就是语言安全故事
的一部分。

```cpp
extern "C" int puts(const char* s);

class Counter {
private:
    int value;
public:
    Counter(int start) {
        this->value = start;
        return;
    }

    int get() const {
        return this->value;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

int main() {
    Counter c(5);
    c.increment();
    c.increment();

    [[scpp::unsafe]] {
        if (c.get() == 7) {
            puts("class keeps behavior with the data");
        }
    }
    return 0;
}
```

输出：

```text
class keeps behavior with the data
```

这个例子虽然小，但已经把分类讲清楚了：

- 数据是私有的；
- 对象通过受检查的方法来操作；
- 以后哪怕把它扩展成真正拥有资源的抽象，也不需要改换语义类别。

这条分界线还带来几个后果：

- **v0.1 没有继承。** `class` 在这一版里首先是“受检查的拥有型对象”，不是一整套
  C++ 对象模型的完整复刻。以后如果真的设计继承，会单独、明确地加回来。
- **`mutable` 是第一阶段的内部可变性。** `class` 里的 `mutable` 字段本身必须
  仍是平凡类型；它可以通过 `const this` 读写，但永远不能被借用，也不能取地址。
  这是 scpp 的“廉价 `Cell<T>` 风格”能力，不是通用逃生舱。
- **按值传参 / 返回复用同一套 copy/move 规则。** scpp 不存在一套“函数边界
  专用、和局部变量不同”的隐藏传输模型。

如果只记住一句话，那就是：**`struct` 的意思是“把我当布局”，`class` 的意思是
“把我当受检查的对象”。**

## 4.3 内存布局与 ABI（固定，不作为 implementation-defined 留白）

只说 `struct`/`class` 的语义分工还不够；真正让 `struct` 在系统编程里有价值的，
是 scpp 对 ABI 的明确承诺。

scpp 把 `struct` 的布局钉死为目标平台的 **Clang ABI**。对同一个 target triple，
一个 scpp `struct` 的字段偏移、padding、对齐、总大小，必须和 Clang 编译等价
C struct 得到的结果逐字节一致。

具体来说：

1. 字段保持源码声明顺序；
2. 每个字段按目标 ABI 的对齐要求放置；
3. 需要时插入 padding；
4. 结构体自己的对齐等于所有字段对齐中的最大值；
5. 总大小向上取整到该对齐的整数倍，保证数组里每个元素都正确对齐。

正因为有这条保证，`struct` 才适合拿来描述包头、FFI 记录、设备描述符，以及
任何“外部代码也必须认同这一串字节怎么解释”的数据。

`class` 则不是 ABI 边界形状。它的职责是进入检查器、携带行为。惯用做法是：
在安全检查覆盖的核心代码里用 `class`，到了边界再转换成 `struct`、裸指针，
或其它 C 兼容表示。

如果某个外部 ABI 确实要求 packed 布局，scpp 提供 `[[scpp::packed]]` 挂在
`struct` / `union` 上
（见 [§5.19](ch05-static-checks.md#519-union-与-scpppacked)）。这就是显式的
布局逃生舱。位域（bit-field）以及通用 `alignas` 风格的用户布局控制，仍不在
v0.1 范围内。

---

[← 上一章：语法糖](ch03-syntactic-sugar.md) · [目录](README.md) · [下一章：静态检查 →](ch05-static-checks.md)
