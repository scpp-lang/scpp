# 函式指標

上一節講的是閉包：它們是小型的區域可呼叫物件。另一個同樣常見、而且更老也更直接的可
呼叫形式，就是**指向具名函式的指標**。

在今天的 scpp 裡，函式指標直接重用普通 C 和 C++ 語法。這表示你可以把它們傳來傳
去、放進物件裡保存，也可以靠目標指標型別來選中某一個重載。

下面每個可執行範例都請存成 `function-pointers.scpp`，然後這樣建置並執行：

```sh
scpp function-pointers.scpp -o function-pointers
./function-pointers
```

## 一個函式名可以初始化匹配的函式指標

普通函式名會退化成一個匹配型別的函式指標值。

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

int main() {
    int (*fp)(int, int) = add;
    int value = fp(2, 3);
    std::println("{}", value);
    return 0;
}
```

輸出：

```text
5
```

這裡最重要的觀念是：`fp` 只是一個值，它保存的是「等一下該呼叫哪一個函式」。透過它呼
叫時，用的仍然是普通函式呼叫語法。

## 函式指標可以存進 class 欄位裡

因為函式指標本來就是普通值，所以 class 完全可以把它保存下來，稍後再使用。

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

class Holder {
public:
    virtual ~Holder() { return; }

private:
    int (*fp_)(int, int) = nullptr;

public:
    Holder(int (*fp)(int, int)) : fp_{fp} {
        return;
    }

    int run(int x, int y) const {
        return this->fp_(x, y);
    }
};

int main() {
    Holder h{&add};
    int value = h.run(4, 5);
    std::println("{}", value);
    return 0;
}
```

輸出：

```text
9
```

這裡的 `&add` 和上面直接寫 `add` 是等價的。有些程式碼在繼續往下傳遞函式時，會更喜
歡這種顯式取位址的寫法。

## 目標指標型別可以選中某一個重載

如果有多個函式共用同一個名字，那麼目標指標型別會決定你指的到底是哪一個。

```cpp
import std;

int encode(int value) {
    return value + 100;
}

char encode(char value) {
    return value;
}

int main() {
    int (*pick_int)(int) = &encode;
    char (*pick_char)(char) = &encode;
    int first = pick_int(23);
    char second = pick_char('Q');
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

輸出：

```text
123
Q
```

所以 `&encode` 自己並不會一直保持含糊不清。只要接收它的目標型別已經明確，重載解析
就知道該選中哪一個具體函式。

## 今天 scpp 裡關於函式指標的工作模型

到目前為止，實用規則可以先記成這樣：

- 函式指標使用普通 C/C++ 的「指向函式的指標」語法；
- 匹配的函式名，或 `&function_name`，都可以用來初始化它；
- 透過函式指標呼叫時，用的仍然是普通呼叫語法；
- 函式指標是普通值，所以可以存進物件裡，也可以在 API 之間傳遞；
- 如果名字本身有重載，目標指標型別會選中匹配的那個重載。

這樣一來，第 11 章就有了繼閉包之後的第二種可呼叫構件。下一節可以再加入擁有型包裝器，
比如 `std::function` 和 `std::move_only_function`。

---

[← 上一章：閉包與捕獲](ch11-01-closures-and-captures.md) · [目錄](README.md)
