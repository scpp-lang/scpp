# 用生命週期驗證參照

上一節說明了：泛型 API 可以描述**一個型別必須支援哪些操作**。但只要 API 接受或回傳
參照，就還會多出一個問題：它要怎樣描述**這些借用是從哪裡來的**？

在今天的 scpp 裡，這件事由 `[[scpp::lifetime(...)]]` 標註負責。它們可以讓泛型函式和
concept probe 描述多個參照參數之間的關係，也可以讓某些 callback 風格 API 表達「這個
參照是新鮮建立出來的，而且只在這次呼叫期間有效」。

下面每個可執行範例都請存成 `lifetimes.scpp`，然後這樣建置並執行：

```sh
scpp lifetimes.scpp -o lifetimes
./lifetimes
```

## 泛型函式可以指明回傳參照來自哪一個輸入參照

當一個泛型函式回傳參照時，生命週期標註可以說明：這個結果到底綁在哪個輸入參照上。

```cpp
import std;

template<typename T>
const T& keep_left(
    const T& left [[scpp::lifetime(a)]],
    const T& right [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return left;
}

int main() {
    int first = 31;
    int second = 8;
    const int& kept = keep_left(first, second);
    int value = kept;
    std::println("{}", value);
    return 0;
}
```

輸出：

```text
31
```

這裡真正重要的不是具體的 `int`，而是這個關係：回傳出來的參照被明確綁到 `a` 組參數，
而不是 `b` 組參數。

## concept probe 也可以檢查多個參照之間的生命週期關係

concept 不只會檢查操作名和回傳型別。probe 也可以描述：多個參照參數之間必須是什麼關
係。

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

class Adder {
public:
    virtual ~Adder() { return; }

public:
    int add(Token& a [[scpp::lifetime(shared)]], Token& b [[scpp::lifetime(shared)]]) const {
        return a.value + b.value;
    }
};

template<typename T>
concept AddsPair = requires(T c, Token& x [[scpp::lifetime(p)]], Token& y [[scpp::lifetime(p)]]) {
    { c.add(x, y) } -> std::same_as<int>;
};

int use_it(const AddsPair auto& adder, Token& left, Token& right) {
    return adder.add(left, right);
}

int main() {
    Adder adder{};
    Token first{};
    first.value = 3;
    Token second{};
    second.value = 4;
    int sum = use_it(adder, first, second);
    std::println("{}", sum);
    return 0;
}
```

輸出：

```text
7
```

這個 probe 把 `x` 和 `y` 都放進 `p` 這一組裡。`Adder::add()` 裡用的是另一個拼寫
`shared`，但這沒有關係：重點是，這兩個宣告裡都表達了「這兩個參數屬於同一個共享組」。

## `[[scpp::lifetime(any)]]` 可以讓 callback 接受一次新鮮建立出來的借用

有些泛型 API 會在自己的函式本體內部建立一個值，然後把它的參照傳給 callback。對這種
模式來說，callback 不可能事先寫出那個具體生命週期的名字，因為這個生命週期是由
callee 當場創造出來的。

```cpp
import std;

class Token {
public:
    virtual ~Token() { return; }

public:
    int value{};
};

template<typename T>
concept AcceptsToken = requires(T callback, Token& tok [[scpp::lifetime(any)]]) {
    { callback(tok) } -> std::same_as<void>;
};

void with_fresh_token(AcceptsToken auto&& callback) {
    Token tok{};
    tok.value = 42;
    callback(tok);
    return;
}

int main() {
    int result = 0;
    with_fresh_token([&result](Token& tok [[scpp::lifetime(any)]]) {
        result = tok.value;
        return;
    });
    std::println("{}", result);
    return 0;
}
```

輸出：

```text
42
```

保留組名 `any` 的意思是「這次呼叫現場新造出來的一個生命週期」。這讓包裝函式可以把自
己區域 `Token` 的參照傳進去，同時 callback 仍然不能把這個借用當成某個更長壽命的命
名分組來使用。

## 今天 scpp 裡關於泛型 API 生命週期的工作規則

到目前為止，實用規則可以先記成這樣：

- 當泛型 API 需要描述借用如何跨呼叫邊界關聯時，就在參照參數上使用 `[[scpp::lifetime(name)]]`；
- 如果回傳參照綁定到某一個特定輸入分組，就寫出匹配的回傳標註；
- concept probe 可以檢查生命週期分組關係，不只是檢查操作名和回傳型別；
- 這些分組名字只在各自宣告內部有意義，所以 probe 裡的 `p` 不需要重用 callee 自己的
  拼寫；
- 對於會把 callee 內部新鮮借用交給 callback 的 API，使用 `[[scpp::lifetime(any)]]`。

這樣一來，第 10 章開頭需要的工具就齊了：泛型定義、concept 描述的操作要求，以及用於
參照的生命週期標註。下一章就可以在這套基礎上繼續講，看看這個專案如何自動驗證行為。

---

[← 上一章：用 concept 描述共享需求](ch10-02-defining-shared-requirements-with-concepts.md) · [目錄](README.md)
