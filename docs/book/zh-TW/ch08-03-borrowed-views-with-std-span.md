# 用 `std::span` 借用視圖

第 4.3 節是從「所有權」的角度引入 `std::span` 的:span 是對一段連續元素的借用,
不是新的擁有者。現在這一章已經把陣列和字元緩衝區擺在桌面上了,就可以再回過頭
來,把 span 看成「某個現有 `T` 緩衝區」的標準函式邊界寫法。

固定大小陣列回答的是「元素存在哪裡?」 span 回答的是「另一個函式怎樣在不複製、
也不接管所有權的前提下使用同一批元素?」

對下面每個可以執行的例子,把檔案存成 `span-buffers.scpp`,然後這樣編譯執行:

```sh
scpp span-buffers.scpp -o span-buffers
./span-buffers
```

對於那些本來就應該被拒絕的例子,如果你想讓編譯器輸出逐字匹配,就按診斷區塊裡
給出的描述性檔名保存。

## 一個 `std::span<const T>` 參數就能讀取不同長度的陣列

借用視圖讓同一個函式可以接收任意長度的固定大小陣列,只要元素型別一致就行。

```cpp
import std;

int sum(std::span<const int> values) {
    int total = 0;
    for (int value : values) {
        total = total + value;
    }
    return total;
}

int main() {
    int first[3]{};
    first[0] = 10;
    first[1] = 20;
    first[2] = 30;

    int second[5]{};
    second[0] = 1;
    second[1] = 2;
    second[2] = 3;
    second[3] = 4;
    second[4] = 5;

    std::println("{} {}", sum(first), sum(second));
    return 0;
}
```

輸出:

```text
60 15
```

這兩個呼叫都沒有複製陣列元素。每次呼叫都只是在邊界處構造出一個小小的唯讀視
圖,然後 `sum` 透過這個視圖去讀資料。

## 可變 span 可以原地改寫借用來的 `char` 緩衝區

上一節用 `char[N]` 建構了和 C 相容的文字緩衝區。如果某個輔助函式需要直接修改那
塊現有緩衝區,它就可以接收 `std::span<char>`。

```cpp
import std;

void shout(std::span<char> text) {
    text[0] = 'S';
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

    shout(word);
    const char* view = word;
    std::println("{}", view);
    return 0;
}
```

輸出:

```text
Scpp!
```

`shout` 並沒有得到這塊緩衝區的所有權。它只是借用了這六個現成字元,並透過這次
借用直接寫了進去。

## span 會把目前借用緩衝區的長度一起帶上

和單獨一個裸指標不同,span 會把元素個數和資料本身一起帶著走。這樣在寫迴圈時,
它就更適合用來保證存取不會跑出借用緩衝區的範圍。

```cpp
import std;

int count_visible(std::span<const char> text) {
    int used = 0;
    while (used < text.size && text[used] != '\0') {
        used = used + 1;
    }
    return used;
}

int main() {
    char title[8]{};
    title[0] = 'b';
    title[1] = 'o';
    title[2] = 'o';
    title[3] = 'k';
    title[4] = '\0';

    std::println("{}", count_visible(title));
    return 0;
}
```

輸出:

```text
4
```

這裡可見文字在第一個 `\0` 就結束了,但它借用的那塊緩衝區本身仍然有八個元素。
`std::span<const char>` 讓函式能同時尊重這兩件事:既能在文字終止符處停下,也不會
越過真實陣列的邊界繼續讀。

## span 不能活得比它借用的儲存更久

因為 span 只是一個視圖,所以如果你試圖回傳一個指向區域陣列的 span,程式會被拒絕。

```cpp
std::span<int> bad() {
    int local[2]{};
    return local;
}

int main() {
    return 0;
}
```

編譯器輸出:

```text
span_return_local_fail.scpp:3:5: error: function 'bad' returns a lifetime-tracked value from an incompatible source type
```

這仍然是前面那個所有權故事,只不過現在應用到了緩衝區視圖上:區域陣列一旦死掉,
指向它的 span 也會立刻懸空,所以 scpp 會直接拒絕這段程式。

## 到目前為止,在面向緩衝區的程式碼裡使用 span 的規則是

- 讓陣列和 `char[N]` 緩衝區繼續做自己那塊儲存的擁有者;
- 當函式只需要讀取一段現有連續緩衝區時,用 `std::span<const T>`;
- 當函式需要原地修改那段借用緩衝區時,用 `std::span<T>`;
- span 會同時帶著起始位址和目前元素個數;
- span 依然服從所有權和生命週期規則,所以它不能活得比自己借用的儲存更久。

有了這一章裡的陣列、文字緩衝區和 span,我們就具備了今天 scpp 裡處理連續資料的
基礎詞彙:擁有型的內聯儲存、和 C 相容的字元緩衝區,以及覆蓋在兩者之上的借用視圖。

---

[← 上一章：把文字當成 `char` 與 C 相容緩衝區處理](ch08-02-text-as-char-and-c-compatible-buffers.md) · [目錄](README.md)
