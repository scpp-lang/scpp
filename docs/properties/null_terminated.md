# `[[scpp::null_terminated]]`

## 作用

`[[scpp::null_terminated]]` 附着在 `char*` 或 `const char*` 类型的函数形参或返回值上，表示该指针非空，且指向一段以 `'\0'` 结尾的有效字符序列。

它用于实现**条件化 `unsafe` 调用**：

- **静态可证时（safe）**：若实参在编译期可被证明满足 `null_terminated`（如字符串字面量 `"hello"`、带 `[[scpp::null_terminated]]` 的形参、或返回类型带 `[[scpp::null_terminated]]` 的函数调用），调用在普通 safe 代码中直接合法。
- **静态不可证时（需要 `unsafe`）**：若实参是来源不明的普通 `char*` / `const char*`（如外部裸指针、`&c`、未证明含 `'\0'` 的数组衰变指针），该调用视为 gated operation，必须放在 `[[scpp::unsafe]]` 中，由调用方保证指针非空且以 `'\0'` 结尾。

注意：`[[scpp::null_terminated]]` 只约束空间上的 `'\0'` 终止条件，不替代借用与生命期检查（如 `std::string_view` 对底层对象的生命期约束仍独立生效）。

## 什么时候用

1. **修饰形参**：函数接收 `const char*` / `char*` 并依赖 `'\0'` 判断结尾（如内部执行 `strlen` 或转发给 C 字符串接口），希望字面量等安全来源可直接调用，同时拦截任意裸指针。
2. **修饰返回值**：函数返回 `const char*` / `char*`，且实现自身保证返回指针非空并以 `'\0'` 结尾（如 `std::string::c_str()`），使下游可继续在 safe 代码中使用。

若接口已显式传入长度（如 `(const char* data, size_t count)` 或 `std::span<const char>`），或指针仅指向单个 `char` 对象，不要使用此属性。

## 示例

```cpp
// 声明
string_view(const char* s [[scpp::null_terminated]]);
const char* c_str() const [[scpp::null_terminated]];

// 调用
std::string_view a{"hello"};       // OK：字符串字面量静态满足 null_terminated
std::string_view b{str.c_str()};   // OK：c_str() 返回值带 null_terminated

[[scpp::unsafe]] {
    std::string_view c{raw_ptr};   // 普通 const char* 必须在 unsafe 中调用
}
```
