# 閉包與捕獲

在泛型函式、concept 與生命週期之後，下一步很自然就是區域 callback 程式碼。

在今天的 scpp 裡，閉包直接重用普通 C++ lambda 語法。最重要的工作模型其實很簡單：閉
包就是一個小型匿名物件，而捕獲列表描述的就是這個物件到底在**存什麼**，或者在**借什
麼**。

下面每個可執行範例都請存成 `closures.scpp`，然後這樣建置並執行：

```sh
scpp closures.scpp -o closures
./closures
```

## 閉包可以傳給一個泛型 callback 參數

最常見的用法，就是把一段短小的區域動作交給另一個函式。

```cpp
import std;

template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

void for_each_doubled(std::span<int> s, IntConsumer auto&& f) {
    int i = 0;
    while (i < s.size) {
        f(s[i] * 2);
        i = i + 1;
    }
    return;
}

int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    int sum = 0;
    for_each_doubled(s, [&sum](int x) { sum = sum + x; });
    std::println("{}", sum);
    return 0;
}
```

輸出：

```text
12
```

這個閉包捕獲了對 `sum` 的可變參照，所以每一次 callback 呼叫，都會更新同一個外層變
數。

## 混合捕獲形式可以讓同一個閉包同時借用一些值、拷貝另一些值

捕獲列表會明確寫出：哪些外部名字會變成按值擁有的欄位，哪些會變成按參照借用的欄位。

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

int main() {
    int a = 5;
    int b = 10;
    int first = apply([&, a](int z) -> int { return a + b + z; }, 3);
    int second = apply([=, &b](int z) -> int { return a + b + z; }, 3);
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

輸出：

```text
18
18
```

`[&, a]` 的意思是「其他名字都按參照借用，但 `a` 自己按值拷貝」。`[=, &b]` 剛好相
反：其他名字都按值拷貝，但 `b` 自己按參照借用。

## 在方法裡，`this` 必須顯式捕獲

在方法內部，閉包當然可以再回呼到 receiver 上，但 `this` 必須明確寫在捕獲列表裡。

```cpp
import std;

template<typename T>
concept IntTransform = requires(T f, int x) { f(x); };

int apply(IntTransform auto&& f, int z) {
    return f(z);
}

class Multiplier {
public:
    virtual ~Multiplier() { return; }

private:
    int factor_{};

public:
    Multiplier(int factor) : factor_{factor} {
        return;
    }

    int scale(int x) const {
        return x * this->factor_;
    }

    int use_closure(int z) {
        return apply([this](int value) -> int { return this->scale(value); }, z);
    }
};

int main() {
    Multiplier m{7};
    int answer = m.use_closure(3);
    std::println("{}", answer);
    return 0;
}
```

輸出：

```text
21
```

這個顯式的 `[this]` 會把生命週期邊界繼續保留在表面上。方法裡的閉包並不會獲得一個繞
過借用規則的隱式後門。

## 今天 scpp 裡關於閉包的工作模型

到目前為止，實用規則可以先記成這樣：

- 最好把閉包理解成一個小型匿名物件；
- 按值捕獲會變成這個物件自己的普通擁有欄位；
- 按參照捕獲會變成借用欄位，所以閉包值會跟其他持有參照的物件一樣，遵守同一套生命週
  期推理；
- 把閉包傳給另一個函式時，通常走的是一個由 concept 約束的普通泛型參數；
- 在方法內部，`this` 必須顯式捕獲。

這已經足夠寫出今天 scpp 裡的普通 callback 風格程式碼了。下一節會繼續留在「可呼叫程
式碼」這個主題裡，但會轉去講具名函式與函式指標。

---

[← 上一章：用生命週期驗證參照](ch10-03-validating-references-with-lifetimes.md) · [目錄](README.md) · [下一章：函式指標 →](ch11-02-function-pointers.md)
