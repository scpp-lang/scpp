# 方法與 `this`

上一節裡，我們把相關資料收進了一個受檢查的 `class`，並且用了像
`area(rectangle)`、`can_hold(outer, inner)` 這樣的自由函式。

這些函式當然能工作，但它們顯然也都是「關於同一個型別」的操作。方法讓我們把這
些行為直接移動到型別本身上。

下面每個可執行範例都可以存成 `methods.scpp`，然後這樣建置並執行：

```sh
scpp methods.scpp -o methods
./methods
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸
出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 定義方法

方法就是寫在 `struct` 或 `class` 定義裡的函式。接收者物件，就是點號前面的那個
值。

```cpp
import std;

class Rectangle {
private:
    std::string name;
    int width{};
    int height{};

public:
    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }

    bool can_hold(const Rectangle& other) const {
        return this->width > other.width && this->height > other.height;
    }

    const char* label() const {
        return this->name.c_str();
    }
};

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.label(), frame.area());
    std::println("{} holds {} = {}", frame.label(), card.label(), frame.can_hold(card));
    return 0;
}
```

輸出：

```text
frame area = 1500
frame holds card = true
```

現在，我們不再寫 `area(frame)`，而是寫 `frame.area()`。這種寫法更清楚地表明：
這個操作屬於 `Rectangle`。

## `this` 指向接收者

在方法內部，`this` 的意思就是「呼叫這個方法的那個物件」。你經常會寫
`this->field` 來把這一點說清楚，尤其是在參數名會和欄位名衝突的時候。

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    void resize(int width, int height) {
        this->width = width;
        this->height = height;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Rectangle rect{2, 3};
    rect.resize(4, 5);
    std::println("{}", rect.area());
    return 0;
}
```

輸出：

```text
20
```

這裡，參數也叫 `width` 和 `height`，所以 `this->width` 的意思就是「接收者上的欄
位」，而不是區域參數。

## `const` 方法只讀取接收者

如果一個方法只需要觀察物件，就把它標成 `const`。這樣一來，這個方法就可以透過
共享借用（例如 `const Rectangle&`）來呼叫。

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int describe(const Rectangle& rectangle) {
    return rectangle.area();
}

int main() {
    Rectangle rect{6, 7};
    std::println("{}", describe(rect));
    return 0;
}
```

輸出：

```text
42
```

這和第 4 章裡的借用模型是同一個故事。`const` 方法面對的是接收者的共享、唯讀視
圖。

## 非 `const` 方法需要可變接收者

如果一個方法可能更新物件，就不要寫 `const`。這樣一來，它就不能透過 `const`
參照來呼叫。

```cpp
class Counter {
private:
    int value{};

public:
    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

void tick(const Counter& counter) {
    counter.increment();
    return;
}

int main() {
    Counter counter{5};
    tick(counter);
    return 0;
}
```

編譯器輸出：

```text
nonconst_method_on_const_ref_fail.scpp:21:5: error: cannot call non-const member function 'increment' through a read-only (const) receiver
 21 |     counter.increment();
    |     ^
```

所以，方法上的 `const` 並不是裝飾。它會改變接收者是如何被借用的，也會改變哪些
呼叫是允許的。

## 呼叫方法會借用接收者

scpp 檢查方法呼叫的方式，和檢查其他借用是一樣的。一個會修改物件的方法，需要拿
到整個接收者物件的可變存取權。

```cpp
class Counter {
public:
    int value{};

    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

int main() {
    Counter counter{5};
    int& value_ref = counter.value;
    counter.increment();
    return value_ref;
}
```

編譯器輸出：

```text
public_field_borrow_conflict.scpp:22:5: error: cannot use 'counter' while it is mutably borrowed
 22 |     counter.increment();
    |     ^
```

對 `counter.value` 的借用一直活到最後那個 `return`，所以 `counter.increment()`
就不能同時再拿到同一個接收者的可變存取權。

下一節會繼續使用方法，但會把重點放到 `[[scpp::unsafe]]` 這樣的所有權邊界上。

---

[← 上一章：一個使用受檢查 class 的示範程式](ch05-02-an-example-program-using-a-checked-class.md) · [目錄](README.md)
