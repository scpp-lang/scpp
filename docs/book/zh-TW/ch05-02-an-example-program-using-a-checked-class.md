# 一個使用受檢查 class 的示範程式

上一節裡，我們學了如何定義 `struct` 和 `class`。這一節，我們用一個小程式把
`class` 真正用起來，讓它的動機更具體一些。

假設我們想處理長方形。一開始，我們也許會把寬度和高度放在分離的變數裡，再寫接
收這些分離參數的自由函式。

下面每個可執行範例都可以存成 `checked-class.scpp`，然後這樣建置並執行：

```sh
scpp checked-class.scpp -o checked-class
./checked-class
```

## 從分離的值開始

這樣寫是可行的，但函式簽名每次都得重複「這兩個值其實是一組」的關係。

```cpp
import std;

int area(int width, int height) {
    return width * height;
}

int main() {
    int width{30};
    int height{50};
    std::println("area = {}", area(width, height));
    return 0;
}
```

輸出：

```text
area = 1500
```

這個程式本身沒有錯。問題在於：`width` 和 `height` 顯然屬於一起，但型別系統現
在還不知道這件事。

## 把這些資料重構成一個 `class`

當幾個相關的值天生屬於一起時，把它們放進同一個型別裡，會讓程式更容易閱讀，也
更不容易意外把它們弄混。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    return 0;
}
```

輸出：

```text
frame area = 1500
```

現在，程式可以用一個名字——`Rectangle`——來表示一件東西，而不是到處傳三個必須
同步的分離值。

這個例子也說明了為什麼在 scpp 裡有時 `class` 才是自然選擇：`Rectangle` 存著一
個 `std::string` 名字，所以它不能是 `struct`。

## 自由函式可以借用整個物件

一旦資料被收進一個值裡，輔助函式就可以接收一個參數，而不是多個分離參數。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

bool can_hold(const Rectangle& outer, const Rectangle& inner) {
    return outer.width > inner.width && outer.height > inner.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    std::println("{} holds {} = {}", frame.name.c_str(), card.name.c_str(), can_hold(frame, card));
    return 0;
}
```

輸出：

```text
frame area = 1500
frame holds card = true
```

注意這些簽名：

- `area(const Rectangle& rectangle)`
- `can_hold(const Rectangle& outer, const Rectangle& inner)`

這些 `const Rectangle&` 參數就是共享借用，和第 4 章裡的參照型別一樣。函式可以
讀取這個長方形，但不會拿走它的所有權。

## 自由函式也可以透過 `T&` 修改物件

如果一個輔助函式需要更新物件，就可以接收 `Rectangle&`。

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

void rename(Rectangle& rectangle, const char* next_name) {
    rectangle.name = std::string{next_name};
    return;
}

int main() {
    Rectangle frame{"draft", 30, 50};
    rename(frame, "published");
    std::println("{}", frame.name.c_str());
    return 0;
}
```

輸出：

```text
published
```

所以，一個受檢查的 `class` 已經能給我們一個很有用的位置來保存相關資料，而普通
函式則負責描述圍繞這些資料的操作。

## 為方法做準備

現在，`area`、`can_hold` 和 `rename` 都還是自由函式。這完全沒有問題；有時這就
是你想要的形態。

但它們也都明顯是在「描述 `Rectangle` 會做什麼」。下一節會把這一類程式再往前推
一步：用方法和 `this` 把這些操作移動到型別自身上。

---

[← 上一章：定義與實例化 `struct` / `class`](ch05-01-defining-and-instantiating-struct-and-class.md) · [目錄](README.md)
