# 什麼是所有權？

所有權是一套規則：它讓 scpp 能在沒有垃圾回收器、也不用你在日常程式裡手寫 `delete` 的情況下，自動完成資源清理。

一旦一個值擁有的不再只是最簡單的純量，而是堆記憶體、檔案句柄、socket，或其他必須「剛好清理一次」的資源，這套規則就開始變得重要。

下面每個短小示例都可以存成 `ownership.scpp`，然後這樣建置並執行：

```sh
scpp ownership.scpp -o ownership
./ownership
```

## 堆疊與堆積

像 `int` 這樣的純量，會直接放在保存它的那個區域變數裡。`std::string` 這樣的值則不同：區域物件本身仍然很小，但它管理的文字放在別處。

`std::string` 會管理存放在堆積上的文字資料，因此它能保存長度在編譯期不必確定、並且可以在執行時增長的文字。

這正是所有權存在的主要原因：總得有誰來決定那塊堆積記憶體該在什麼時候釋放。

可以先把這套模型記成三條實用規則：

- 每一份被擁有的資源，都由某一個仍然活著的擁有物件負責；
- `std::move(x)` 會把這份責任從 `x` 轉移出去，並立刻讓 `x` 進入 moved-out 狀態；
- 目前擁有者離開作用域時，清理會自動發生。

## 作用域決定清理何時發生

最基本的所有權概念，就是作用域。一個區域物件從宣告處開始有效，一直到包住它的那個程式區塊結束。

```cpp
import std;

class Note {
private:
    const char* name{};

public:
    Note(const char* text) : name{text} {
        std::println("start {}", this->name);
        return;
    }

    ~Note() {
        std::println("drop {}", this->name);
        return;
    }
};

int main() {
    std::println("before inner");
    {
        Note inner{"inner"};
        std::println("inside inner");
    }
    std::println("after inner");
    return 0;
}
```

輸出：

```text
before inner
start inner
inside inner
drop inner
after inner
```

`inner` 在執行走到它的宣告處時建立，而它的解構函式會在內層程式區塊結束時自動執行。這就是 scpp 裡最普通的 RAII 故事：作用域決定清理何時發生。

## `std::string` 擁有堆積上的資料

`std::string` 很適合拿來當第一個所有權示例，因為它的大小可以在執行時改變。

```cpp
import std;

int main() {
    std::string title{"scpp"};
    title.append(" book");

    std::println("{} ({} bytes)", title.c_str(), title.length());
    return 0;
}
```

輸出：

```text
scpp book (9 bytes)
```

區域變數 `title` 擁有這個 `std::string` 值。由於這個字串會管理配置在堆積上的文字資料，所以當 `title` 離開作用域時，它的解構函式就會釋放那塊記憶體。

## move 會轉移所有權

當一個擁有型值應該改由別人負責時，就用 `std::move`。

```cpp
import std;

int main() {
    std::string first{"owner"};
    std::string second{std::move(first)};
    second.append("ship");

    std::println("{}", second.c_str());
    return 0;
}
```

輸出：

```text
ownership
```

在 scpp 裡，`std::move(first)` 不只是一個函式庫輔助函式。語言本身把這種語法當成「立刻把 `first` 置為 moved-out」的操作。到了這一步之後，`first` 就不能再用了，而 `second` 成了那個字串物件唯一還活著的擁有者。

這就是 scpp 避免雙重解構的方式：一旦所有權已經移走，舊擁有者既不會再被使用，也不會在作用域結束時按「已初始化物件」再次解構。

## 複製和 move 是兩回事

有些值會被複製，而不是被 move。像 `int`、`bool`、`char`、`double` 這樣的普通純量就是如此。

```cpp
import std;

int main() {
    int x{5};
    int y{x};
    y = y + 1;

    std::println("x = {}", x);
    std::println("y = {}", y);
    return 0;
}
```

輸出：

```text
x = 5
y = 6
```

修改 `y` 不會影響 `x`，因為這裡發生的是值複製。

對於 class 型別，複製並不是自動存在的。一個 class 只有真的定義了複製行為，才是可複製的。`std::string` 現在已經支援深拷貝，所以普通的複製建構和複製指定都會得到一個新的擁有型字串值。

像 `std::string second{first};` 這樣的大括號初始化會呼叫 copy constructor，而 `third = first;` 會呼叫 copy assignment：

```cpp
import std;

int main() {
    std::string first{"book"};
    std::string second{first};
    std::string third{"draft"};
    third = first;
    second.append(" chapter");
    third.append(" notes");

    std::println("first = {}", first.c_str());
    std::println("second = {}", second.c_str());
    std::println("third = {}", third.c_str());
    return 0;
}
```

輸出：

```text
first = book
second = book chapter
third = book notes
```

`second` 和 `third` 都各自擁有自己的字串值。修改任何一個副本，都不會影響 `first`。

## 所有權與函式

按值傳參和按值回傳，遵循的是同一套所有權規則。

class 值可以透過按值回傳，把所有權交回給 caller。函式寫成 `return std::move(word);` 時，所有權會從這個區域值轉移到回傳值上：

```cpp
import std;

std::string make_word() {
    std::string word{"hello"};
    return std::move(word);
}

int main() {
    std::string local{make_word()};
    std::println("{}", local.c_str());
    return 0;
}
```

輸出：

```text
hello
```

`make_word()` 回傳的是一個擁有型 `std::string`，而 caller 裡的 `local` 會成為新的擁有者。

如果一個 class 值透過 `std::move` 按值傳進函式，那麼 callee 也會接管這個實參的所有權：

```cpp
import std;

void print_word(std::string text) {
    std::println("{}", text.c_str());
    return;
}

int main() {
    std::string word{"hello"};
    print_word(std::move(word));
    return 0;
}
```

輸出：

```text
hello
```

在 `std::move(word)` 之後，形參 `text` 就成了 `print_word` 裡面那個仍然活著的擁有者。

如果某個 class 型別**有**複製行為，那麼把一個左值按值傳進函式時，就可以透過複製建立出新的擁有物件：


```cpp
import std;

class Label {
private:
    const char* text{};

public:
    Label(const char* value) : text{value} {
        return;
    }

    Label(const Label& other) : text{other.text} {
        std::println("copy {}", this->text);
        return;
    }

    const char* c_str() const {
        return this->text;
    }
};

Label echo_label(Label label) {
    return label;
}

int main() {
    Label first{"ticket"};
    Label second{echo_label(first)};
    std::println("{}", second.c_str());
    return 0;
}
```

輸出：

```text
copy ticket
ticket
```

這次執行只印出了一次 `copy ticket`：把 `first` 按值傳進去時，會先把它複製到形參物件裡。之後回傳這份區域值時，語言仍然可以選擇 move 它或直接重用它；但核心點不變：只要型別可複製，函式邊界就能在程式顯式進行複製時建立出新的擁有者。

到這裡，第一版所有權模型就夠用了：

- 作用域結束，會結束擁有者的生命週期；
- `std::move` 會轉移所有權，並讓舊擁有者失效；
- 普通純量會便宜地複製，而 class 型別只有真的定義了複製行為時才可複製；
- 函式邊界要麼 move 所有權，要麼按型別定義複製出一個新的擁有者。

下一節會保持這套所有權規則不變，但開始回答一個新問題：如果程式只想暫時使用某個值，而不想拿走它的所有權，該怎麼辦？

---

[← 上一章：控制流程](ch03-05-control-flow.md) · [目錄](README.md) · [下一章：參照與借用 →](ch04-02-references-and-borrowing.md)
