# 定義與實例化 `struct` / `class`

`struct` 或 `class` 能把一組相關欄位收攏到同一個名字下面。這樣一來，程式就不
用到處傳好幾個分離的值，而是可以先定義一個「這些欄位天生屬於一起」的型別。

在 scpp 裡，`struct` 和 `class` **不是**可互換的兩種寫法：

- `struct` 是 plain-data、非多型的那一種。它仍然可以有 `public:` /
  `private:` 區段、建構函式，以及普通的非虛成員函式。
- `class` 則是為繼承與多型準備的那一種形式。它也可以持有 `std::string` 這樣
  的非平凡 class 型別欄位。

這種劃分是刻意為之的。選擇 `struct`，等於在說「這個型別不會進入繼承、
interface 與虛呼叫的世界」。選擇 `class`，則是從一開始就明確進入那個世界：每
一個 `class` 都必須宣告 `virtual` 解構函式，即使它什麼都不做。這樣一來，之後
再加入虛函式或 interface base 時，就不會悄悄改變這個型別的形狀；同時，scpp
也避免了 C++ 裡那種「把類別當成基底類別使用，卻忘了寫 virtual 解構函式」的經
典陷阱。

後面的章節會詳細介紹繼承和 interface。眼下先記住一條分界線：只有 `class` 能
參與那套系統。`struct` 不能宣告虛成員，不能寫 base-clause，也不能標成
`[[scpp::interface]]`；反過來，一個用 `struct` 宣告出來的型別，後面也不能被某
個 `class` 拿去當 base。

下面每個可執行範例都可以存成 `records.scpp`，然後這樣建置並執行：

```sh
scpp records.scpp -o records
./records
```

對於那些本來就應該被編譯器拒絕的範例，如果你希望得到與書裡逐字一致的診斷輸
出，請把檔案存成診斷區塊裡顯示的那個描述性檔名。

## 定義一個帶命名欄位的基礎 `struct`

只有欄位的 `struct`，是把相關資料放在一起的最簡單方式。

```cpp
import std;

struct User {
    int id{};
    const char* name{""};
};

int main() {
    User user{};
    user.id = 7;
    user.name = "Ada";
    std::println("{} {}", user.id, user.name);
    return 0;
}
```

輸出：

```text
7 Ada
```

`User user{};` 會建立一個 `User` 值，並先對欄位做預設初始化。之後，欄位就可以
用普通的點語法來讀寫。

在目前的 scpp 裡，像 `User user{7, "Ada"};` 這樣帶參數的大括號，並不會自動把
值填進 public 欄位裡。如果你想在建構時就傳參數，就要定義建構函式。

## `struct` 仍然可以隱藏欄位並定義行為

在 scpp 裡，如果你只是想隱藏資料或定義建構函式，**並不需要**因此切到 `class`。
`struct` 仍然可以有 `private:` 區段、default constructor、parameterized
constructor，以及普通的非虛成員函式。

```cpp
import std;

struct Size {
private:
    int width{};
    int height{};

public:
    Size() {
        return;
    }

    Size(int initial_width, int initial_height)
        : width{initial_width}, height{initial_height} {
        return;
    }

    void grow_width(int delta) {
        this->width = this->width + delta;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Size empty{};
    Size window{3, 4};
    window.grow_width(1);
    std::println("{} {}", empty.area(), window.area());
    return 0;
}
```

輸出：

```text
0 16
```

這裡的 `Size` 仍然是一個 `struct`，儘管它把欄位藏了起來，還圍繞這些欄位定義了
行為。我們會在第 5.3 節再回到方法語法；現在更重要的點是：只要一個型別應該保
持「非虛、非繼承」的資料形態，`struct` 仍然就是那個普通而正統的工具。

## 單參數建構函式也可以在呼叫點觸發轉換

單參數建構函式還可以充當 converting constructor。這代表：如果一個函式按值接收
該型別，那麼呼叫點也可以直接傳入那個建構參數。

```cpp
import std;

struct Meters {
    int value{};

public:
    Meters(int initial_value) : value{initial_value} {
        return;
    }
};

int read(Meters meters) {
    return meters.value;
}

int main() {
    Meters direct{8};
    std::println("{} {}", read(5), direct.value);
    return 0;
}
```

輸出：

```text
5 8
```

這仍然是普通的建構。`read(5)` 之所以可行，是因為 scpp 會先從 `5` 建構一個臨時
的 `Meters`，再用它來滿足這個按值參數。

## `struct` 的欄位必須保持 plain data

scpp 裡的 `struct` 仍然只接受 plain-data 欄位型別。如果某一個欄位需要
`std::string` 這樣的 class 行為，那麼外層這個型別就必須改成 `class`。

```cpp
import std;

struct Bad {
    std::string name{"hi"};
};

int main() {
    Bad value{};
    return 0;
}
```

編譯器輸出：

```text
struct_string_field_fail.scpp: error: struct 'Bad' field 'name': a class type 'std::string' cannot be a struct field; use class instead (only scalars, pointers, trivial structs/unions, and fixed-size arrays of trivial types are allowed here; see spec ch04)
```

這也是它和普通 C++ 的一大差別：在普通 C++ 裡，`struct` 和 `class` 往往主要只差
預設存取層級；但在 scpp 裡，它們承諾的能力邊界本身就不同。

## 定義並實例化一個 `class`

在使用點上，`class` 看起來仍然很熟悉：你同樣會定義欄位、用大括號建構一個值，
並且用點語法存取 public 欄位。

```cpp
import std;

class DisplayName {
public:
    std::string text;

    DisplayName(const char* initial_text) : text{initial_text} {
        return;
    }

    virtual ~DisplayName() {
        return;
    }
};

int main() {
    DisplayName name{"scpp"};
    std::println("{}", name.text.c_str());
    return 0;
}
```

輸出：

```text
scpp
```

表面語法很簡單，但這裡的設計選擇已經和 `struct` 不一樣了。這個型別現在可以持
有 `std::string`，而且因為它是 `class`，它也進入了語言裡「之後可以有一個普通
base class，再加任意多個 interface base」的那一側。

## 每個 `class` 都必須明確宣告 virtual 解構函式

如果你省掉這個解構函式，那麼即使這個類別沒有別的虛成員，程式也仍然是
ill-formed 的。

```cpp
class Account {
public:
    Account() {
        return;
    }
};

int main() {
    Account account{};
    return 0;
}
```

編譯器輸出：

```text
class_without_virtual_dtor_fail.scpp: error: class 'Account' must declare an explicit virtual destructor (spec §11.5(1))
```

所以在 scpp 裡，選擇 `class` 並不只是風格偏好。它是語言裡專門留給繼承與多型
的那種形式，而解構函式規則就是讓這個選擇從一開始就明確、穩定的一部分。

## 預設大括號初始化仍然需要 default constructor

`Type value{};` 的意思是「用零個建構參數來建構一個值」。如果一個型別只宣告了帶
參數建構函式，那麼這種初始化會被正常地以「建構函式選擇失敗」的方式拒絕。

```cpp
struct CtorOnly {
    int value;

public:
    CtorOnly(int x) : value{x} {
        return;
    }
};

int main() {
    CtorOnly value{};
    return 0;
}
```

編譯器輸出：

```text
struct_default_ctor_fail.scpp:11:5: error: type 'CtorOnly' has no default constructor; no constructor of 'CtorOnly' matches 0 arguments
 11 |     CtorOnly value{};
    |     ^
```

同樣的規則也適用於 `class`。如果你希望 `Type value{};` 成立，那麼這個型別就真
的必須有 default constructor。

## `struct` 不能宣告虛成員

反過來，`struct` 這一側的限制也同樣重要：`struct` 永遠不是虛的。

```cpp
struct Plain {
    virtual void f() {
        return;
    }
};

int main() {
    Plain value{};
    return 0;
}
```

編譯器輸出：

```text
struct_virtual_member_fail.scpp:2:5: error: a declaration introduced by 'struct' shall not declare a virtual member function or virtual destructor (spec §11.1(2.3))
 2 |     virtual void f() {
   |     ^
```

如果一個型別需要 virtual 行為，它就必須是 `class`。

## `struct` 不能繼承

同樣地，在 scpp 裡，`struct` 也不是拿來繼承的那種形式。

```cpp
class Base {
public:
    Base() {
        return;
    }

    virtual ~Base() {
        return;
    }
};

struct Derived : public Base {
    Derived() {
        return;
    }
};

int main() {
    Derived value{};
    return 0;
}
```

編譯器輸出：

```text
struct_inherit_fail.scpp:12:16: error: a declaration introduced by 'struct' shall not have a base-clause (spec §11.1(2.1))
 12 | struct Derived : public Base {
    |                ^
```

同一條邊界在線的另一側也成立：後面某個 `class` 也不能把 `struct` 當作 base，而
且 `struct` 也不能標成 `[[scpp::interface]]`。如果某個型別將來可能參與繼承或
interface，就應該從一開始把它定義成 `class`。

## `struct` 與 `class` 規則小結

到這裡，工作規則可以整理成：

- 當一個型別應該保持 plain-data、非虛、非繼承時，用 `struct` 來組織相關資料；
- `struct` 仍然可以有 `public:` / `private:` 區段、建構函式，以及普通的非虛成
  員函式；
- 單參數建構函式可以在呼叫點充當 converting constructor；
- 無論是 `struct` 還是 `class`，欄位都用點語法存取；
- 如果你想寫 `Type value{};`，這個型別就真的必須有 default constructor；
- `struct` 不能持有 `std::string` 這樣的 ownership-tracked 欄位，不能宣告虛成
  員，不能寫 base-clause，也不能成為 interface；
- 每一個 `class` 都必須明確宣告一個 `virtual` 解構函式；
- 在 scpp 裡，只有 `class` 才會參與繼承、虛呼叫，以及 interface 實作。

下一節會圍繞一個「受檢查的 `class`」搭一個小示範程式。

---

[← 上一章：`std::span` 與其他非擁有視圖](ch04-03-std-span-and-other-non-owning-views.md) · [目錄](README.md)
