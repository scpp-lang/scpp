# 用 concept 描述共享需求

上一節說明了：一個泛型定義可以被很多具體型別實例化。接下來的問題是：API 要怎樣表
達**自己期待哪一類型別**？

在今天的 scpp 裡，這件事由 `concept` 負責。concept 會替一組操作取一個可重複使用的
名字，而泛型 API 可以要求呼叫端提供真正支援這些操作的型別。

下面每個可執行範例都請存成 `concepts.scpp`，然後這樣建置並執行：

```sh
scpp concepts.scpp -o concepts
./concepts
```

## concept 會替結構性的需求命名

一個型別之所以滿足某個 concept，是因為它真的擁有那些被要求的操作。這裡沒有單獨的
「把這個型別登記成實作了某個 concept」宣告。

```cpp
import std;

class Circle {
public:
    virtual ~Circle() { return; }

    int area() const {
        return 314;
    }
};

class Square {
public:
    virtual ~Square() { return; }

    int area() const {
        return 100;
    }
};

template<typename T>
concept Shape = requires(const T& t) {
    { t.area() } -> std::same_as<int>;
};

int area_value(const Shape auto& shape) {
    return shape.area();
}

int main() {
    Circle circle{};
    Square square{};
    int circle_area = area_value(circle);
    int square_area = area_value(square);
    std::println("{}", circle_area);
    std::println("{}", square_area);
    return 0;
}
```

輸出：

```text
314
100
```

這個 concept 精確寫出了函式本體真正需要的東西：給定 `const T&` 之後，呼叫 `area()`
必須合法，而且結果必須是 `int`。`Circle` 和 `Square` 都在結構上滿足這個要求，所以
這兩個呼叫都能通過編譯。

## 當同一個受約束型別要出現多次時，完整模板標頭形式更合適

縮寫形式 `Concept auto` 很適合只有一個參數的時候；如果同一個型別參數要出現在多個位
置，完整模板標頭會更清楚。

```cpp
import std;

class Meter {
public:
    virtual ~Meter() { return; }

private:
    int value_{};

public:
    Meter(int value) : value_{value} {
        return;
    }

    int get() const {
        return this->value_;
    }
};

template<typename T>
concept HasGet = requires(const T& t) {
    { t.get() } -> std::same_as<int>;
};

template<HasGet T>
int sum_pair(const T& left, const T& right) {
    return left.get() + right.get();
}

int main() {
    Meter first{19};
    Meter second{23};
    int total = sum_pair(first, second);
    std::println("{}", total);
    return 0;
}
```

輸出：

```text
42
```

這裡 concept 仍然是在描述共享需求，但這個函式需要一個被命名出來的 `T`，因為兩個參
數必須擁有同一個具體型別。

## 泛型 class 可以讓部分操作保持不受約束，再用 `requires` 打開額外能力

有時候，一個泛型 class 應該接受很多型別，但只有某一個方法需要額外能力。這時就把約
束寫在那個方法本身上。

```cpp
import std;

template<typename T>
concept Describable = requires(const T& t) {
    { t.magnitude() } -> std::same_as<int>;
};

class Circle {
public:
    virtual ~Circle() { return; }

private:
    int radius_{};

public:
    Circle(int radius) : radius_{radius} {
        return;
    }

    Circle(const Circle& other) : radius_{other.radius_} {
        return;
    }

    int magnitude() const {
        return this->radius_;
    }
};

template<typename T>
class Box {
public:
    virtual ~Box() { return; }

private:
    T item_;

public:
    Box(const T& item) : item_{item} {
        return;
    }

    const T& get() const {
        return this->item_;
    }

    int describe() const requires Describable<T> {
        return this->item_.magnitude();
    }
};

int main() {
    Box<int> number{5};
    Box<Circle> circle{Circle{9}};
    int number_value = number.get();
    int circle_value = circle.describe();
    std::println("{}", number_value);
    std::println("{}", circle_value);
    return 0;
}
```

輸出：

```text
5
9
```

`Box<T>` 本身仍然保持廣泛可用。`get()` 對任何 `T` 都成立；而 `describe()` 只會在那
個實例化出來的 `T` 滿足 `Describable` 時才可用。

## 今天 scpp 裡關於 concept 的工作規則

到目前為止，實用規則可以先記成這樣：

- 用 `template<typename T> concept Name = requires(...) { ... };` 來定義 concept；
- 一個型別是透過支援那些被要求的操作，在結構上滿足這個 concept；
- 只有一個受約束參數時，用 `Concept auto`；
- 如果同一個受約束型別參數要出現在多個位置，就改用完整模板標頭形式；
- 如果只有泛型 class 的某個方法需要額外能力，就把 `requires Concept<T>` 寫在那個方
  法上。

這樣一來，第 10 章就多了一塊新的詞彙：泛型 API 不再只會表達「這裡放某種型別」，還
能表達「這裡放某種**支援這些操作**的型別」。下一節會再加入生命週期，讓這些共享需求
也能安全地談論參照。

---

[← 上一章：泛型函式與 class](ch10-01-generic-functions-and-classes.md) · [目錄](README.md) · [下一章：用生命週期驗證參照 →](ch10-03-validating-references-with-lifetimes.md)
