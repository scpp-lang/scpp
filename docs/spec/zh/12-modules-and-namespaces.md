# 10 模块与命名空间

## 10.1 总则 [module.unit], [namespace.def]

(1) 除本条款明确修改的部分外，[module.unit] 到 [module.private.frag]，
以及 [namespace.def] 到 [namespace.alias]，都原样适用于 SCPP26 程序。
SCPP26 原样复用 C++26 的 module 与 namespace 语法——`export module`、
`module`、`import`、`export` 与 `namespace`，包括嵌套的
*namespace-definition* 形式 `namespace A::B::C { ... }`——不引入任何
新的关键字、运算符或其他 token。

(2) 本条款要求每一个被导出的声明都出现在由其自身模块名字决定的命名
空间里（[§10.3](#103-导出声明与所需命名空间-moduleinterface)），
为 *import-declaration* 给出精确的可见性规则，包括模块自身 partition
的可见性规则
（[§10.4](#104-导入声明重新导出与跨模块名字合并-moduleimport)），
撤销了 C++26 本来允许的两种命名空间形式
（[§10.5](#105-被禁止的命名空间形式-namespaceunnamed-namespaceudir)），
并且限制了 *postfix-expression* 为非限定名字的函数调用的名字查找
（[§10.6](#106-非限定函数调用的名字查找-basiclookupunqual-basiclookupargdep)）。

## 10.2 模块声明形式与 partition [module.unit]

(1) 一个翻译单元，如果它的第一条声明是 `export module`
*module-name*`;`，那么它就是该模块的**主接口单元**（primary interface
unit）。一个模块恰好只有一个主接口单元。

(2) *module-name* 后面可以跟一个 `:` 加上 *partition-name*，把该翻译
单元指定为该模块的一个 **partition**，它既不同于该模块的主接口单元，
也不同于该模块的其他任何 partition：

  (2.1) `export module` *module-name*`:`*partition-name*`;` 声明的是
  一个 **interface partition**，它自己也可以导出声明
  （[§10.3](#103-导出声明与所需命名空间-moduleinterface)）；

  (2.2) `module` *module-name*`:`*partition-name*`;`（前面没有
  `export`）声明的是一个 **implementation partition**，它不能导出任何
  声明；对这样一个 partition 使用 *export-import-declaration*
  （[§10.4](#104-导入声明重新导出与跨模块名字合并-moduleimport)）是不
  合法的。

(3) partition 是其所属模块的一部分，但不能在该模块之外按名字导入：
只有本身就以 `export module` *module-name*`;` 开头，或者以
`module`/`export module` *module-name*`:`*other-partition-name*`;`
（对同一个 *module-name*）开头的翻译单元，才能包含一条命名了该模块某
个 partition 的 *import-declaration*。

【注：不严谨地说，只有本身就属于模块 M 的文件——它的主接口单元，或者
它的任意一个 partition——才能 `import` M 的其他 partition；M 之外的
文件只能把 M 作为一个整体来 `import`
（[§10.4](#104-导入声明重新导出与跨模块名字合并-moduleimport)）。——注释结束】

```cpp
// geometry.scpp —— 模块 "geometry" 的主接口单元
export module geometry;

export import :distance;   // 重新导出下面这个 partition，见 §10.4

namespace geometry {
    export int scale(int x) { return x * 10; }
}
```

```cpp
// geometry_distance.scpp —— 同一个模块的一个 interface partition
export module geometry:distance;

namespace geometry {
    export int manhattan(int ax, int ay, int bx, int by) {
        int dx = ax - bx; if (dx < 0) dx = -dx;
        int dy = ay - by; if (dy < 0) dy = -dy;
        return dx + dy;
    }
}
```

```cpp
// main.scpp —— 模块 "geometry" 之外的一个翻译单元
import geometry;

int main() {
    return geometry::manhattan(0, 0, 3, 4) + geometry::scale(2);  // 27
}
```

## 10.3 导出声明与所需命名空间 [module.interface]

(1) `export` 施加于某个声明（[module.interface]）时，会让它对导入该
模块的翻译单元可见——或者，如果这是 partition 里的声明，则是对导入了
该 partition 的翻译单元可见
（[§10.4](#104-导入声明重新导出与跨模块名字合并-moduleimport)）。没有
被 `export` 施加的声明，对其所属模块而言是**私有**（private）的：
在同一个模块内部的其他地方仍然可以（按普通查找规则允许的方式，用限定
名或非限定名）访问到它，但永远无法通过 *import-declaration* 访问到它。

(2) 如果 `export` 施加于某个声明，而该声明所在翻译单元的第一条声明
不属于 10.2(1) 或 10.2(2.1) 中的任何一种形式，那么程序不合法。

【注：只有主接口单元或者 interface partition 才能导出任何东西。
implementation partition 不能（10.2(2.2)），而完全没有 module 声明的
翻译单元，压根就没有可以从中导出东西的模块。——注释结束】

(3) 把 *module-name* 按 `.` 拆分，将拆分出来的每一段标识符都当作再往
里嵌套一层，由此得到的命名空间记作 ns(*module-name*)（于是
ns(`org.lotx.cmath`) 表示命名空间 `org::lotx::cmath`）；对于 partition，
ns(*module-name*) 按同样的方式，从其 *module-name* 中 `:` 之前的部分
推导得到。如果 `export` 施加于某个声明，那么该声明必须是
ns(*module-name*) 的成员，或者是嵌套在 ns(*module-name*) 内部（不论
嵌套多少层）的某个命名空间的成员；否则程序不合法。

【注：(2) 与 (3) 是两个各自独立、都必须满足的条件：一个满足 (3) 但
所在翻译单元不满足 (2) 的声明仍然不合法，反过来也一样。一个是
ns(*module-name*) 真前缀的命名空间，或者在任何一层上和它发生分歧的
命名空间，都不满足 (3)，即使它可能和 ns(*module-name*) 共享一段
开头的标识符序列也一样。——注释结束】

(4) 没有被 `export` 施加的声明不受 (3) 约束，可以是任何命名空间的
成员，也可以不属于任何命名空间。

```cpp
export module org.lotx.cmath;

namespace org::lotx::cmath {
    export int abs_int(int x) { return x < 0 ? -x : x; }   // OK：(3)

    namespace detail {
        export int clamp(int x, int lo, int hi) {          // OK：(3)，
            return x < lo ? lo : (x > hi ? hi : x);         // 嵌套更深
        }
    }
}

namespace org::lotx {
    int helper() { return 0; }               // OK：(4)，没有被导出

    export int wrong() { return 1; }         // 不合法：(3)，'org::lotx'
                                               // 是一个真前缀，不是
                                               // ns(module-name) 或更深
}

export int no_namespace_at_all() { return 2; }  // 不合法：(3)
```

## 10.4 导入声明、重新导出与跨模块名字合并 [module.import]

(1) 一条普通的 *import-declaration*——`import` *module-name*`;`，或者
在模块 M 内部对 M 自己某个 partition 使用的
`import :`*partition-name*`;`——只让被导入单元导出的声明在发起导入的
这个翻译单元里可见；它不具有传递性。导入某个自身又（以普通方式）导入
了第三个模块或 partition 的东西，并不会因此获得那个第三方模块或
partition 的可见性。

(2) 一条 *export-import-declaration*——`export import` *module-name*`;`，
或者在模块 M 的某个 interface partition 或主接口单元内部使用的
`export import :`*partition-name*`;`——具有和 (1) 相同的可见性效果，
并且还会让由此变得可见的每一个声明，对导入了这条
*export-import-declaration* 所在单元的任何翻译单元，也传递地可见。

(3) 在模块 M 内部，`import :`*partition-name*`;` 会让该 partition 的
每一个声明——不论是否被 `export` 施加——都变得可见，但仅对发起导入的
这个翻译单元可见，不具有传递性。如果该 partition 是一个
implementation partition（10.2(2.2)），那么
`export import :`*partition-name*`;` 不合法。

【注：因此，对一个 partition 的普通 `import`，和按名字对另一个模块的
普通 `import`，行为并不一样：前者会把一个 partition 的全部声明——包括
私有的——暴露在同一个模块内部，后者则只暴露该模块自身导出的内容 (1)。
——注释结束】

(4) 一个通过一条或多条 *import-declaration* 变得对某翻译单元可见的
声明，会被合并进该翻译单元的命名空间，效果就好像它是在同一个翻译单元
里、处在相同的命名空间作用域位置上被声明的一样。如果这样合并起来的
两个声明，或者这样一个声明和发起导入的翻译单元自身的某个声明，假如
真的都写在同一个翻译单元里、会构成不合法的重新声明或重新定义
（[basic.def.odr]、[dcl.fct]），那么程序就不合法；否则它们就会一起参与
重载决议（[over.match]），效果和同一个翻译单元内部同名的多个声明——
无论签名相同还是不同——参与重载决议完全一样。

【注：并不存在专门针对"因 import 而产生歧义的名字"的诊断；一对发生
冲突的被导入声明不合法的原因，和诊断方式，跟这两者假如直接写在同一个
文件里完全一样。当两个模块各自所需的命名空间
（[§10.3](#103-导出声明与所需命名空间-moduleinterface) 的 (3)）因为
嵌套关系而相关——比如因为其中一个模块的名字是另一个模块名字的
前缀——它们就可能各自合法地导出签名不同、限定名相同的声明，并按 (4)
合并进同一个重载集合。——注释结束】

```cpp
// org.scpp —— 模块 "org" 的主接口单元
export module org;

namespace org::lotx {
    export int describe(int x) { return x; }
}
```

```cpp
// org_lotx.scpp —— 模块 "org.lotx" 的主接口单元
export module org.lotx;

namespace org::lotx {
    // OK：(4)，一个合法的重载，不是冲突
    export int describe(int x, int y) { return x + y; }
}
```

```cpp
// main.scpp
import org;
import org.lotx;

int main() {
    return org::lotx::describe(5) + org::lotx::describe(10, 27);  // 42
}
```

## 10.5 被禁止的命名空间形式 [namespace.unnamed], [namespace.udir]

(1) 一个 *unnamed-namespace-definition*（[namespace.unnamed]）是不
合法的。

(2) 一条 *using-directive*（[namespace.udir]）是不合法的。

【注：在某个 class 的 *member-specification* 里，命名了某个 base
class 成员的 *using-declaration*（[namespace.udecl]）不受 (2) 影响，
依然是合法的（见 [§11](11-inheritance-and-interfaces.md)）；(2) 只针对
命名了某个命名空间的 *using-directive*。——注释结束】

```cpp
namespace {
    int x = 0;      // 不合法：(1)
}

namespace foo {
    int y = 0;
}

using namespace foo;   // 不合法：(2)
```

## 10.6 非限定函数调用的名字查找 [basic.lookup.unqual], [basic.lookup.argdep]

(1) 对任何函数调用，都不会执行参数依赖查找（[basic.lookup.argdep]）。

(2) 设 S 为最内层、包住某个函数调用之处的命名空间，该调用的
*postfix-expression* 是一个命名该函数的非限定 *id-expression*（为此，
忽略任何介于其间的 block、function、class 或 lambda 作用域）。对该
*id-expression* 的非限定查找（[basic.lookup.unqual]）只考虑：

  (2.1) 在包住该调用之处的 block、函数形参或 class 作用域里能找到的
  声明，和未修改的 C++ 完全一样；

  (2.2) S 的直接成员声明；以及

  (2.3) 全局命名空间的直接成员声明。

不考虑任何其他命名空间，即使它在词法上包住 S 也一样；具体来说，除了
全局命名空间之外，任何包住 S 的命名空间都永远不会被搜索，尽管
[basic.lookup.unqual] 本来也会检查它。

【注：这条限制只针对为函数调用自身的被调用者名字所执行的非限定查找。
对类型、变量或者其他非调用用途的名字所做的非限定查找不受影响，依然会
和未修改的 C++ 一样，检查包住该用法之处的每一层命名空间。*qualified-id*
同样不受影响，包括以 `::` 开头的（[basic.lookup.qual]）：不论 (2)，
`N::f()` 和 `::f()` 都和未修改的 C++ 一样被查找。——注释结束】

```cpp
int global_helper() { return 100; }

namespace outer {
    int outer_helper() { return 10; }

    namespace inner {
        struct Widget { int v; };

        int use_it() {
            Widget w{};              // OK：非限定类型查找不受 (2)
            w.v = 1;                 // 影响，正常地向外查到 'outer'
            return global_helper();  // OK：(2.3)
        }

        int bad() {
            return outer_helper();   // 不合法：(2)；'outer' 既不是
        }                             // (2) 里的 S（'inner'），也不是
    }                                  // 全局命名空间
}
```

```cpp
namespace ns {
    struct Tag { int v; };
    int process(Tag t) { return t.v; }
}

int use(ns::Tag t) {
    return process(t);   // 不合法：(1)；没有参数依赖查找，即使
}                          // 'process' 和 'Tag' 都是 'ns' 的成员
```

---

[← 上一节：继承与接口](11-inheritance-and-interfaces.md) · [目录](README.md)
