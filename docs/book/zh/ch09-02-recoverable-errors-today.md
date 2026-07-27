# 今天可用的可恢复错误写法

第 9.1 节讲的是当前 scpp 会视为“不可恢复”的失败:一旦某个受检操作发现状态不对,
程序就会立刻中止。

但不是所有问题都属于这一类。有时候,一个函数更合理的做法只是说“我现在产不出这
个值”,然后把后续怎么处理交给调用方。今天标准库里处理这种情况的主要工具就是
`std::optional<T>`。

optional 要么包含一个 `T` 值,要么什么都不包含。这很适合查找、解析步骤,以及那
些把“没找到”视为正常结果而不是致命 bug 的操作。

对下面每个可以运行的例子,把文件保存成 `optional.scpp`,然后这样编译运行:

```sh
scpp optional.scpp -o optional
./optional
```

对于那些本来就应该中止的例子,程序会成功编译,但一旦它去向一个空 optional 索要
根本不存在的值,运行就会结束。

## 当函数“可能有值、也可能没值”时,返回 `std::optional<T>`

函数可以返回 optional,然后由调用方对 `has_value()` 分支处理。

```cpp
import std;

std::optional<int> find_score(int id) {
    if (id == 7) {
        std::optional<int> full{99};
        return full;
    }
    std::optional<int> empty{};
    return empty;
}

int main() {
    std::optional<int> found = find_score(7);
    std::optional<int> missing = find_score(1);

    if (found.has_value()) {
        std::println("score = {}", found.value());
    }
    if (!missing.has_value()) {
        std::println("not found");
    }
    return 0;
}
```

输出:

```text
score = 99
not found
```

这里最重要的变化是谁来做决定。`find_score` 在没有答案时不会中止程序;它只会返
回一个空 optional,再由调用方决定怎样处理这个完全正常的情况。

## 就算 `T` 不能默认构造,空 optional 也照样成立

即使 `T` 自己必须带构造参数,`std::optional<T>` 仍然可以处于空状态。

```cpp
import std;

class Ticket {
private:
    int id_{};

public:
    virtual ~Ticket() = default;

    Ticket(int id) : id_{id} {
        return;
    }

    Ticket(const Ticket& other) : id_{other.id_} {
        return;
    }

    int id() const {
        return this->id_;
    }
};

int main() {
    std::optional<Ticket> empty{};
    std::optional<Ticket> full{Ticket{42}};
    std::println("{} {}", empty.has_value(), full->id());
    return 0;
}
```

输出:

```text
false 42
```

`Ticket` 自己没有零参数构造函数,但 optional 仍然可以通过“里面什么都不放”来表达
“现在还没有 ticket”。

## `reset()` 会清掉当前值,另一个 optional 可以把它再填回来

一个 optional 一旦有了值,你可以先把它清回空状态,再用另一个 optional 把它替换掉。

```cpp
import std;

int main() {
    std::optional<int> answer{7};
    std::println("{} {}", answer.has_value(), answer.value());

    answer.reset();
    std::println("{}", answer.has_value());

    std::optional<int> replacement{9};
    answer = replacement;
    std::println("{}", answer.value());
    return 0;
}
```

输出:

```text
true 7
false
9
```

这就形成了一个很直接的状态机:有值、空、再变回有值。关键在于,每一次状态变化都
由调用方显式写出来。

## 对空 optional 调 `value()` 仍然是一次受检失败

`std::optional` 之所以能把“缺失”变成可恢复情况,前提是代码真的先检查再访问。对
空 optional 直接调用 `value()` 会中止程序。

```cpp
import std;

int main() {
    std::optional<int> empty{};
    return empty.value();
}
```

运行时行为:程序会直接中止,因为 `empty` 里面根本没有 `int`。

所以 `std::optional<T>` 不是“没有规则的可空数据”。真正可恢复的部分是显式检查
`has_value()`,而不是无条件去取值。

## 到目前为止,今天可用的可恢复错误模式是

- 当一个函数本来就可能没有 `T` 可返回时,用 `std::optional<T>`;
- 在调用 `value()` 之前,先检查 `has_value()`;
- 即使 `T` 没有默认构造函数,optional 也照样能表达“缺失”;
- `reset()` 会把 optional 变回空状态;
- 今天的 `std::optional<T>` 只能告诉你“有没有值”,不能告诉你“为什么没有”。

最后这个限制正好通向下一节:怎样把 API 先设计成适合以后升级到“同时携带值和错
误说明”的结果类型。

---

[← 上一章：不可恢复错误与编译器插入的检查](ch09-01-unrecoverable-errors-and-compiler-inserted-checks.md) · [目录](README.md)
