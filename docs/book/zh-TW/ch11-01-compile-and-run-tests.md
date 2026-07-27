# 編譯並執行式測試

到第 10 章結束時，我們已經有了足夠的語言工具來寫出有用的函式庫程式碼。接下來的問
題就是：怎樣把這些程式碼持續自動檢查起來。

這個專案的第一層測試，就是 `blackbox_test/` 下面的黑盒執行器。它把 `scpp` 當成一個
外部命令列編譯器來呼叫，去建置小型 `.scpp` 程式、執行它們，再把實際結果和期望結果
檔做比較。

假設主編譯器建置已經產出了 `./build/scpp`，那麼在倉庫根目錄下這樣建置測試執行器：

```sh
cmake -S blackbox_test -B blackbox_test/build
cmake --build blackbox_test/build
```

## 一個成功測試，本質上就是一個 `.scpp` 檔配一個 `.expected` 檔

先建一個分類目錄，再放進去一個很小的程式：

`blackbox_test/cases/99_demo/hello_test.scpp`

```cpp
import std;

int main() {
    std::println("hello from test");
    return 0;
}
```

然後在旁邊放上期望結果：

`blackbox_test/cases/99_demo/hello_test.expected`

```text
0
hello from test
```

第一行是行程退出碼。第一行換行之後的所有內容，就是執行器期望看到的精確 stdout。

現在只執行這一小塊測試：

```sh
./blackbox_test/build/run_tests 99_demo
```

輸出：

```text
ok   99_demo/hello_test.scpp

1/1 case(s) passed.
```

這就是這個倉庫裡編譯並執行式測試的核心模式：寫一個展示某條規則的小程式，再寫出它經
過 `scpp` 編譯並執行之後**應該**得到的精確結果。

## 反向測試使用 `COMPILE_ERROR`

有些文件化規則本來就應該拒絕程式碼，而不是讓它執行。這時 `.expected` 檔就直接把這
件事寫出來。

`blackbox_test/cases/99_demo/const_ref_rejects_write.scpp`

```cpp
int main() {
    int value = 7;
    const int& ref = value;
    ref = 9;
    return 0;
}
```

`blackbox_test/cases/99_demo/const_ref_rejects_write.expected`

```text
COMPILE_ERROR
```

再跑一次同樣的過濾：

```sh
./blackbox_test/build/run_tests 99_demo
```

輸出：

```text
ok   99_demo/const_ref_rejects_write.scpp
ok   99_demo/hello_test.scpp

2/2 case(s) passed.
```

`COMPILE_ERROR` **不會**固定要求某一條精確診斷文字。它表達的只是：編譯器必須以一個
真正的錯誤**乾淨地失敗**，而不是崩潰，也不是錯誤地接受這個程式。

## 關於黑盒編譯並執行式測試的工作模型

到目前為止，實用規則可以先記成這樣：

- 每個普通黑盒測試，都是一對同名相鄰檔案：`<name>.scpp` 和 `<name>.expected`；
- 如果 `.expected` 以一個數字開頭，這個數字就是期望退出碼，後面的位元組就是期望 stdout；
- 如果 `.expected` 是 `COMPILE_ERROR`，那這個程式就必須被乾淨地拒絕；
- 執行器接受子字串過濾，所以迭代時可以只重跑一個分類，或一小組相關測試。

這已經足夠寫出這個倉庫裡最常見的測試：要麼是編譯並執行後有確定結果的小程式，要麼是
必須因為某條語言規則而編譯失敗的程式。下一節會繼續加入額外輔助檔案，來控制 CLI 參數、
輸出路徑以及其他命令細節。

---

[← 上一章：用生命週期驗證參照](ch10-03-validating-references-with-lifetimes.md) · [目錄](README.md)
