# 把文字當成 `char` 與 C 相容緩衝區處理

第 8.1 節先把「固定大小陣列」這件事本身講清楚了。這一節把同樣的思路收窄到
`char`:字串字面量、原始 C 字串,以及把位元組直接存在自己內部的可寫緩衝區。

這種底層表示在今天的 scpp 裡依然很重要。`std::string` 已經存在,而且在你需要一
個可增長、自己擁有的文字值時很好用;但很多 API -- 尤其是 `extern "C"` 邊界 --
今天仍然直接使用 `const char*` 和 `char[N]`。

對下面每個可以執行的例子,把檔案存成 `char-buffers.scpp`,然後這樣編譯執行:

```sh
scpp char-buffers.scpp -o char-buffers
./char-buffers
```

對於那些本來就應該被拒絕的例子,如果你想讓編譯器輸出逐字匹配,就按診斷區塊裡
給出的描述性檔名保存。

## 字串字面量是唯讀的 `const char*`

字串字面量給你的是一段借用來的唯讀文字。

```cpp
import std;

int main() {
    const char* greeting = "hello";
    std::println("{} {}", greeting, greeting[1]);
    return 0;
}
```

輸出:

```text
hello e
```

`greeting` 並不擁有一塊由 `main` 自己配置出來的緩衝區。它只是一個指向現成唯讀
位元組的指標,而對這個指標做索引存取,讀到的就是其中單個字元。

## `char[N]` 陣列會把可寫文字緩衝區直接擁有在自己內部

當你需要一塊可以由目前函式自己修改的位元組儲存時,固定大小 `char` 陣列就跟第
8.1 節裡的其他陣列一樣運作。額外要記住的一條規則是:C 風格文字必須以 `\0` 結
尾,這樣另一個 API 才知道文字在哪裡結束。

```cpp
extern "C" int puts(const char* s);

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '!';
    word[5] = '\0';

    [[scpp::unsafe]] {
        puts(word);
    }
    return 0;
}
```

輸出:

```text
scpp!
```

`word` 直接擁有六個 `char` 元素。對 `puts` 的呼叫要放進 `[[scpp::unsafe]]` 裡,
原因跟本書裡其他 `extern "C"` 呼叫一樣:編譯器看不到對面真正的實作。

最後那個 `\0` 和前面的可見字元一樣重要。`puts` 會一直往後讀,直到碰到這個終止
符為止。

## 把 `char[N]` 緩衝區傳給函式時,改到的仍然是同一塊儲存

在呼叫邊界上,陣列緩衝區可以傳給一個寫成陣列語法的參數,而被呼叫函式會直接原地
改寫呼叫方那塊緩衝區。

```cpp
import std;

void make_excited(char text[6]) {
    text[4] = '!';
    return;
}

int main() {
    char word[6]{};
    word[0] = 's';
    word[1] = 'c';
    word[2] = 'p';
    word[3] = 'p';
    word[4] = '?';
    word[5] = '\0';

    make_excited(word);
    const char* text = word;
    std::println("{}", text);
    return 0;
}
```

輸出:

```text
scpp!
```

`make_excited` 並沒有收到一個新複製出來的陣列。它寫到的仍然是同一塊底層緩衝
區,所以呼叫方裡的 `word[4]` 也跟著變了。

## `std::string` 負責擁有可增長文字,`c_str()` 負責接到 C API 上

當大小一開始就確定時,固定大小 `char[N]` 緩衝區很好用。要是你需要一個自己擁
有、而且還能增長的文字值,就用 `std::string`,只在真正需要的邊界上借出 C 相容視
圖。

```cpp
import std;

extern "C" int puts(const char* s);

int main() {
    std::string name{"scpp"};
    name.append(" book");

    [[scpp::unsafe]] {
        puts(name.c_str());
    }

    int letters = static_cast<int>(name.size());
    std::println("{}", letters);
    return 0;
}
```

輸出:

```text
scpp book
9
```

這裡文字的所有權一直都在 `name` 手裡,不在那個 C API 手裡。`c_str()` 給出的就
是 `puts` 需要的、借用來的 nul 結尾指標,而在普通 scpp 程式碼裡,`std::string`
仍然是處理動態長度文字時更合適的工具。

## 字串字面量不能直接初始化可變 `char*`

字面量本來就是唯讀文字,所以 scpp 不允許你把它當成可寫緩衝區。

```cpp
int main() {
    char* text = "hi";
    return 0;
}
```

編譯器輸出:

```text
string_literal_mutable_pointer_fail.scpp:2:18: error: cannot initialize or assign raw pointer 'text' from an incompatible pointer type without an explicit cast
```

如果一段程式碼打算透過這個指標寫資料,那它就必須真的拿到可寫儲存 -- 比如它自己
擁有的 `char[N]` 陣列。如果只是讀取字面量,那就改用 `const char*`。

## 到目前為止,文字緩衝區的規則是

- 字串字面量是借用來的唯讀文字,通常寫成 `const char*`;
- 可寫的 C 風格文字緩衝區通常是一個固定大小 `char[N]` 陣列,並且要明確寫上結尾
  的 `\0`;
- 把這個陣列傳給別的函式時,對方拿到的是對同一組底層位元組的指標式存取;
- 當文字需要增長時,`std::string` 是更合適的擁有型工具,而 `c_str()` 可以把一個
  借用來的 C 相容指標交給別的 API;
- 字串字面量不能直接初始化可變 `char*`。

下一節會回到「借用視圖」這個主題,用 `std::span` 來表達「看著一塊連續緩衝區,但
並不接管它的所有權」。

---

[← 上一章：固定大小陣列](ch08-01-fixed-size-arrays.md) · [目錄](README.md)
