# 编译并运行式测试

到第 10 章结束时，我们已经有了足够的语言工具来写出有用的库代码。接下来的问题就是：
怎样把这些代码持续自动检查起来。

这个项目的第一层测试，就是 `blackbox_test/` 下面的黑盒运行器。它把 `scpp` 当成一个
外部命令行编译器来调用，去构建小型 `.scpp` 程序、运行它们，再把实际结果和期望结果
文件做比较。

假设主编译器构建已经产出了 `./build/scpp`，那么在仓库根目录下这样构建测试运行器：

```sh
cmake -S blackbox_test -B blackbox_test/build
cmake --build blackbox_test/build
```

## 一个成功测试，本质上就是一个 `.scpp` 文件配一个 `.expected` 文件

先建一个分类目录，再放进去一个很小的程序：

`blackbox_test/cases/99_demo/hello_test.scpp`

```cpp
import std;

int main() {
    std::println("hello from test");
    return 0;
}
```

然后在旁边放上期望结果：

`blackbox_test/cases/99_demo/hello_test.expected`

```text
0
hello from test
```

第一行是进程退出码。第一行换行之后的所有内容，就是运行器期望看到的精确 stdout。

现在只运行这一小块测试：

```sh
./blackbox_test/build/run_tests 99_demo
```

输出：

```text
ok   99_demo/hello_test.scpp

1/1 case(s) passed.
```

这就是这个仓库里编译并运行式测试的核心模式：写一个展示某条规则的小程序，再写出它经
过 `scpp` 编译并执行之后**应该**得到的精确结果。

## 反向测试使用 `COMPILE_ERROR`

有些文档化规则本来就应该拒绝代码，而不是让它运行。这时 `.expected` 文件就直接把这件
事写出来。

`blackbox_test/cases/99_demo/const_ref_rejects_write.scpp`

```cpp
int main() {
    int value = 7;
    const int& ref = value;
    ref = 9;
    return 0;
}
```

`blackbox_test/cases/99_demo/const_ref_rejects_write.expected`

```text
COMPILE_ERROR
```

再跑一次同样的过滤：

```sh
./blackbox_test/build/run_tests 99_demo
```

输出：

```text
ok   99_demo/const_ref_rejects_write.scpp
ok   99_demo/hello_test.scpp

2/2 case(s) passed.
```

`COMPILE_ERROR` **不会**固定要求某一条精确诊断文字。它表达的只是：编译器必须以一个
真正的错误**干净地失败**，而不是崩溃，也不是错误地接受这个程序。

## 关于黑盒编译并运行式测试的工作模型

到目前为止，实用规则可以先记成这样：

- 每个普通黑盒测试，都是一对同名相邻文件：`<name>.scpp` 和 `<name>.expected`；
- 如果 `.expected` 以一个数字开头，这个数字就是期望退出码，后面的字节就是期望 stdout；
- 如果 `.expected` 是 `COMPILE_ERROR`，那这个程序就必须被干净地拒绝；
- 运行器接受子串过滤，所以迭代时可以只重跑一个分类，或一小组相关测试。

这已经足够写出这个仓库里最常见的测试：要么是编译并运行后有确定结果的小程序，要么是
必须因为某条语言规则而编译失败的程序。下一节会继续加入额外辅助文件，来控制 CLI 参数、
输出路径以及其他命令细节。

---

[← 上一章：用生命周期验证引用](ch10-03-validating-references-with-lifetimes.md) · [目录](README.md)
