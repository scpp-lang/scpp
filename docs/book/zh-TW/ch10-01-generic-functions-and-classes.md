# 泛型函式與 class

第 10 章從一個最基本的觀察開始：有時我們希望**同一種程式形狀**能適用於不止
一種具體型別。

在今天的 scpp 裡，起點就是熟悉的 C++ template 語法。一個泛型定義會寫出一個或多
個型別參數，而每一次具體使用都會用真正的型別去實例化它。

下面每個可執行範例都請存成 `generics.scpp`，然後這樣建置並執行：

```sh
scpp generics.scpp -o generics
./generics
```

## 一個泛型函式可以同時抽象多個不同的具體參數型別

函式模板可以把一種操作寫一次，然後讓不同呼叫點各自選擇自己的具體型別。

```cpp
import std;

template<typename T, typename U>
T first(T left, U right) {
    return left;
}

int main() {
    std::println("{} {}", first(42, true), first('S', 9));
    return 0;
}
```

輸出：

```text
42 S
```

這兩個呼叫並不需要先統一成某一個共同參數型別。第一次呼叫裡，`T = int`、
`U = bool`；第二次呼叫裡，`T = char`、`U = int`。一個泛型函式頭就覆蓋了這兩種具
體情況。

## 一個泛型 class 可以存放實例化時指定的具體型別值

泛型 class 使用同樣的 `template<typename ...>` 標頭，然後在欄位和方法裡直接使用這
些型別參數。

```cpp
import std;

template<typename T>
class Holder {
public:
    virtual ~Holder() { return; }

private:
    T item_;

public:
    Holder(const T& item) : item_{item} {
        return;
    }

    T get() const {
        return this->item_;
    }
};

int main() {
    Holder<int> number{42};
    Holder<char> initial{'G'};
    std::println("{} {}", number.get(), initial.get());
    return 0;
}
```

輸出：

```text
42 G
```

`Holder<int>` 和 `Holder<char>` 來自同一個 class 定義，但在最終程式裡，它們仍然是
兩個不同的、已經實例化好的具體型別。

## 每個實例化都可以分別解析依賴型別參數的資料版面配置

泛型 class 並不只是「一個在執行時抹掉型別資訊的通用盒子」。不同實例化可以產生
不同的具體儲存版面配置。

```cpp
import std;

template<typename T>
class Buffer {
public:
    virtual ~Buffer() { return; }

    char storage_[sizeof(T)]{};

    int size() const {
        return static_cast<int>(sizeof(this->storage_));
    }
};

struct OneByte {
    char value;
};

struct EightBytes {
    int left;
    int right;
};

int main() {
    Buffer<int> ints{};
    Buffer<OneByte> one{};
    Buffer<EightBytes> pair{};
    std::println("{} {} {}", ints.size(), one.size(), pair.size());
    return 0;
}
```

輸出：

```text
4 1 8
```

所以 `Buffer<int>`、`Buffer<OneByte>` 和 `Buffer<EightBytes>` 並不是都在共用一模一
樣的版面配置。每一個實例化都會針對自己的具體 `T`，分別解析出 `sizeof(T)`。

## 第 10 章開頭關於泛型程式碼的工作模型

到目前為止，實用規則可以先記成這樣：

- 用普通的 `template<typename ...>` 標頭來寫泛型函式和泛型 class；
- 每一次具體使用，都會把這些型別參數替換成真實型別；
- 一個泛型定義可以服務很多不同的具體呼叫，或很多不同的 class 實例化型別；
- 依賴型別參數的欄位和方法本體，會針對每個實例化分別解析；
- 下一節會加入 concept，它能讓 API 不只表達「這裡放某種型別」，還表達「這種型
  別必須支援這些操作」。

這樣一來，本章後面部分就有了共同基礎：泛型函式家族、泛型 class 家族，以及由它
們各自產生出來的具體實例化型別。

---

[← 上一章：如何在 fail-fast、`std::optional<T>` 與 `std::expected<T, E>` 之間做選擇](ch09-06-choosing-between-fail-fast-optional-and-expected.md) · [目錄](README.md)
