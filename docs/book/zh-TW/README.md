# SCPP 程式語言

- [開始使用](ch01-00-getting-started.md)
  - [建置編譯器](ch01-01-building-the-compiler.md)
  - [Hello, World!](ch01-02-hello-world.md)
  - [第一個 project build](ch01-03-hello-project-builds.md)

- [做一個猜數字小遊戲](ch02-00-guessing-game.md)

- 常見程式設計概念
  - [變數與明確初始化](ch03-01-variables-and-explicit-initialization.md)
  - [純量資料型別](ch03-02-scalar-data-types.md)
  - [函式](ch03-03-functions.md)
  - [註解](ch03-04-comments.md)
  - [控制流程](ch03-05-control-flow.md)

- 理解所有權
  - [什麼是所有權？](ch04-01-what-is-ownership.md)
  - [參照與借用](ch04-02-references-and-borrowing.md)
  - [`std::span` 與其他非擁有視圖](ch04-03-std-span-and-other-non-owning-views.md)

- 用 struct 與 class 組織相關資料
  - [定義與實例化 `struct` / `class`](ch05-01-defining-and-instantiating-struct-and-class.md)
  - [一個使用受檢查 class 的示範程式](ch05-02-an-example-program-using-a-checked-class.md)
  - [方法與 `this`](ch05-03-methods-and-this.md)

- 安全邊界與 `[[scpp::unsafe]]`
  - [`[[scpp::unsafe]]` 會做什麼、不會做什麼](ch06-01-what-scpp-unsafe-does-and-does-not-do.md)
  - [呼叫 `extern "C"` 與使用裸指標](ch06-02-calling-extern-c-and-using-raw-pointers.md)
  - [如何把「信任」局部化到真實程式裡](ch06-03-localizing-trust-in-real-programs.md)

- 套件、模組與專案版面配置
  - 套件與專案清單
  - 用模組控制作用域與可見性
  - 在模組樹中參照項目的路徑
  - 使用 `import` 與限定名稱
  - 把模組拆到不同檔案中

- 陣列、緩衝區與視圖
  - 固定大小陣列
  - 把文字當成 `char` 與 C 相容緩衝區處理
  - 用 `std::span` 借用視圖

- 錯誤處理
  - 不可恢復錯誤與編譯器插入的檢查
  - 目前可用的可恢復錯誤寫法
  - 為 `std::expected` 做準備

- 泛型程式碼、concept 與生命週期
  - 泛型資料型別
  - 用 concept 描述共享需求
  - 用生命週期驗證參照

- 撰寫自動化測試
  - 編譯並執行式測試
  - 控制測試命令
  - 測試組織方式

- 一個 I/O 專案：打造命令列程式
  - 接收命令列參數
  - 讀取檔案
  - 重構成模組
  - 用測試增加功能
  - 處理環境變數
  - 把診斷訊息寫到標準錯誤

- 閉包與顯式迭代
  - 閉包
  - 用迴圈和視圖處理一串資料
  - 繼續改進命令列專案
  - 顯式迴圈的效能

- 進一步理解 project build 與可重用套件
  - 編譯器模式與專案建置模式
  - 建構可重用的模組產物
  - workspace
  - 安裝與執行二進位程式
  - 擴充工具鏈

- 智慧指標與擁有型句柄
  - 使用 `std::unique_ptr<T>`
  - 像用參照一樣使用擁有型指標
  - 用解構函式執行清理程式碼
  - `std::shared_ptr<T>`
  - 用 `mutable` 做內部可變性
  - 避免參照環與所有權混亂

- 無畏並行
  - 用執行緒同時執行程式碼
  - 安全地跨執行緒移動資料
  - 共享狀態並行
  - 執行緒 trait：`thread_movable` 與 `thread_shareable`

- 互通性與固定版面配置資料
  - 固定版面配置的 `struct` 值
  - C ABI 邊界
  - packed 版面配置與 `union` 逃生艙

- 進階特性
  - 進階 concept 與約束
  - 進階型別與函式指標
  - 進階函式與閉包
  - 沒有巨集時如何做中繼程式設計

- 最終專案：打造一個多執行緒 Web 伺服器
  - 先做單執行緒 Web 伺服器
  - 從單執行緒走向多執行緒
  - 優雅關閉與清理

- 附錄
  - A - Attribute 與保留寫法
  - B - 運算子與符號
  - C - 標準函式庫基礎元件
  - D - 常用開發工具
  - E - SCPP26 與 C++26 基線
  - F - 本書翻譯版本
