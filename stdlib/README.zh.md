# stdlib —— scpp 的 `std` 标准库

这个目录保存 scpp 自己实现的标准库：`std` 模块主接口单元、各个分区，
以及这些分区在需要时依赖的原生辅助库。

本项目在标准库上的约定是：

- 通过 scpp 真实的模块系统暴露库接口（`import std;`）
- 按分区/功能拆到各自子目录（`string/`、`memory/`、未来更多分区）
- 能用纯 scpp 实现的就用纯 scpp
- 只有当某个分区必须桥接到 scpp 目前尚未自行实现的现有运行时能力时，
  才增加很小的原生 C/C++ 包装层

## 目录结构

| 路径 | 作用 |
|---|---|
| `std.scpp` | `std` 模块的主接口单元；通过 `export import :...;` 重新导出各分区 |
| `string/` | `std:string` 分区，以及桥接真实 C++ `std::string` 的原生 `scpp_string_wrapper` |
| `memory/` | `std:memory` 分区；目前是纯 scpp（`std::unique_ptr`、`std::make_unique`） |
| `CMakeLists.txt` | 构建各 stdlib 分区所需的原生辅助库 |

## 如何使用 `std`

用户代码写：

```cpp
import std;
```

构建时显式传入模块映射，例如：

```sh
scpp app.scpp -o app \
  --import std=stdlib/std.scpp \
  --import std:string=stdlib/string/std_string.scpp \
  --import std:memory=stdlib/memory/std_memory.scpp \
  --link build/stdlib/libscpp_string_wrapper.a
```

说明：

- `std.scpp` 负责聚合各个分区；源码消费者不应直接在代码里写
  `import std:string;` 或 `import std:memory;`
- 只有需要原生辅助库的分区才需要 `--link`。当前只有 `std:string`
  需要；`std:memory` 是纯 scpp，不需要额外原生库
- 各分区会和主接口单元一起编译成同一个 `std` 模块目标文件，不存在文本拼接

## 当前分区

### `std:string`

- 文件：`string/std_string.scpp`
- 底层实现：`string/scpp_string_wrapper.{h,cpp}`
- 通过 `extern "C"` 包装函数提供一小部分 `std::string` 能力

### `std:memory`

- 文件：`memory/std_memory.scpp`
- 纯 scpp 实现
- 提供 `std::unique_ptr<T>` 和 `std::make_unique<T>(...)`

## 测试原则

`stdlib/` 是库源码目录，不是演示程序目录。行为覆盖应该进入真正的测试套件：

- `tests/`：dev-agent 负责的单元/集成测试
- `blackbox_test/`：面向用户可见行为的黑盒测试

原生辅助库仍然在这里构建，因为编译器和测试都需要它们；但演示可执行程序
不应继续放在这里。
