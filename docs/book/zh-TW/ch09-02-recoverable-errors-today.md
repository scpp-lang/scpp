# 目前可用的可恢復錯誤寫法

第 9.1 節講的是目前 scpp 會視為「不可恢復」的失敗:一旦某個受檢操作發現狀態不對,
程式就會立刻中止。

但不是所有問題都屬於這一類。有時候,一個函式更合理的做法只是說「我現在產不出這
個值」,然後把後續怎麼處理交給呼叫方。今天標準函式庫裡處理這種情況的主要工具就
是 `std::optional<T>`。

optional 要麼包含一個 `T` 值,要麼什麼都不包含。這很適合查找、解析步驟,以及那
些把「沒找到」視為正常結果而不是致命 bug 的操作。

對下面每個可以執行的例子,把檔案存成 `optional.scpp`,然後這樣編譯執行:

```sh
scpp optional.scpp -o optional
./optional
```

對於那些本來就應該中止的例子,程式會成功編譯,但一旦它去向一個空 optional 索要
根本不存在的值,執行就會結束。

## 當函式「可能有值、也可能沒值」時,回傳 `std::optional<T>`

函式可以回傳 optional,然後由呼叫方對 `has_value()` 分支處理。

```cpp
import std;

std::optional<int> find_score(int id) {
    if (id == 7) {
        std::optional<int> full{99};
        return full;
    }
    std::optional<int> empty{};
    return empty;
}

int main() {
    std::optional<int> found = find_score(7);
    std::optional<int> missing = find_score(1);

    if (found.has_value()) {
        std::println("score = {}", found.value());
    }
    if (!missing.has_value()) {
        std::println("not found");
    }
    return 0;
}
```

輸出:

```text
score = 99
not found
```

這裡最重要的變化是誰來做決定。`find_score` 在沒有答案時不會中止程式;它只會回
傳一個空 optional,再由呼叫方決定怎樣處理這個完全正常的情況。

## 就算 `T` 不能預設建構,空 optional 也照樣成立

即使 `T` 自己必須帶建構參數,`std::optional<T>` 仍然可以處於空狀態。

```cpp
import std;

class Ticket {
private:
    int id_{};

public:
    virtual ~Ticket() = default;

    Ticket(int id) : id_{id} {
        return;
    }

    Ticket(const Ticket& other) : id_{other.id_} {
        return;
    }

    int id() const {
        return this->id_;
    }
};

int main() {
    std::optional<Ticket> empty{};
    std::optional<Ticket> full{Ticket{42}};
    std::println("{} {}", empty.has_value(), full->id());
    return 0;
}
```

輸出:

```text
false 42
```

`Ticket` 自己沒有零參數建構函式,但 optional 仍然可以透過「裡面什麼都不放」來表
達「現在還沒有 ticket」。

## `reset()` 會清掉目前值,另一個 optional 可以把它再填回來

一個 optional 一旦有了值,你可以先把它清回空狀態,再用另一個 optional 把它替換掉。

```cpp
import std;

int main() {
    std::optional<int> answer{7};
    std::println("{} {}", answer.has_value(), answer.value());

    answer.reset();
    std::println("{}", answer.has_value());

    std::optional<int> replacement{9};
    answer = replacement;
    std::println("{}", answer.value());
    return 0;
}
```

輸出:

```text
true 7
false
9
```

這就形成了一個很直接的狀態機:有值、空、再變回有值。關鍵在於,每一次狀態變化都
由呼叫方明確寫出來。

## 對空 optional 呼叫 `value()` 仍然是一次受檢失敗

`std::optional` 之所以能把「缺失」變成可恢復情況,前提是程式碼真的先檢查再存取。
對空 optional 直接呼叫 `value()` 會中止程式。

```cpp
import std;

int main() {
    std::optional<int> empty{};
    return empty.value();
}
```

執行時行為:程式會直接中止,因為 `empty` 裡面根本沒有 `int`。

所以 `std::optional<T>` 不是「沒有規則的可空資料」。真正可恢復的部分是明確檢查
`has_value()`,而不是無條件去取值。

## 到目前為止,今天可用的可恢復錯誤模式是

- 當一個函式本來就可能沒有 `T` 可回傳時,用 `std::optional<T>`;
- 在呼叫 `value()` 之前,先檢查 `has_value()`;
- 即使 `T` 沒有預設建構函式,optional 也照樣能表達「缺失」;
- `reset()` 會把 optional 變回空狀態;
- 今天的 `std::optional<T>` 只能告訴你「有沒有值」,不能告訴你「為什麼沒有」。

最後這個限制正好通向下一節:怎樣把 API 先設計成適合以後升級到「同時攜帶值和錯
誤說明」的結果型別。

---

[← 上一章：不可恢復錯誤與編譯器插入的檢查](ch09-01-unrecoverable-errors-and-compiler-inserted-checks.md) · [目錄](README.md)
