# libs —— scpp 的库模块

这个目录保存 scpp 随附的库模块：`libs/std/` 下是真实 `std` 模块，
`libs/scpp/` 下是 scpp 自己扩展出来的模块；需要时也会包含配套的原生辅助库。

本项目在标准库上的约定是：

- 通过 scpp 真实的模块系统暴露库接口（`import std;`）
- 按分区/功能拆到各自子目录（`string/`、`memory/`、未来更多分区）
- 能用纯 scpp 实现的就用纯 scpp
- 只有当某个分区必须桥接到 scpp 目前尚未自行实现的现有运行时能力时，
  才增加很小的原生 C/C++ 包装层

## 目录结构

| 路径 | 作用 |
|---|---|
| `std/scpp.toml` | `std` 包的 manifest：`[[lib]]` 源集合，加上 `[additional_objs.std-native]` 包装层对象构建步骤 |
| `std/std.scpp` | `std` 模块的主接口单元；通过 `export import :...;` 重新导出各分区 |
| `std/` | `std` 模块的各分区和原生包装库 |
| `scpp/scpp.toml` | `scpp` 包的 manifest；通过 path dependency 依赖 `../std` |
| `scpp/scpp.scpp` | `scpp` 模块的主接口单元；重新导出 scpp 自己的扩展分区 |
| `scpp/rand/` | `scpp:rand` 分区，提供 `scpp::rand::uniform_int_distribution<int>` |

## Manifest workspace

现在 `libs/` 作为仓库顶层 workspace manifest（`../scpp.toml`，从
`libs/scpp.toml` 上移一层，这样它也能覆盖 `src/` 和
`applications/httpserver/`，见 `../src/scpp.toml` 和
`../applications/httpserver/scpp.toml`）的两个成员，使用书里教给用户的
manifest-based flow：

- 根目录的 `scpp.toml` workspace 声明了
  `members = ["libs/std", "libs/scpp", "src", "applications/httpserver"]`，
  没有设置 `default-members`，所以从仓库根目录直接执行 `scpp build`
  （不带 `-p`/`--workspace`）以及下面提到的顶层 `CMakeLists.txt` 里的
  custom command，都会构建全部四个成员包，包括 `src` 的 `ast.cppm`
  self-hosting 目标和 `applications/httpserver` 的 `[[bin]]` 目标
- `libs/std/scpp.toml` 定义 `std` 这个库包
- `libs/scpp/scpp.toml` 定义 `scpp` 这个库包，并通过 path dependency 依赖 `std`

现在 `libs/` 已经把 wrapper 编译也放进 manifest build 本身：

- `[[lib]]` 声明 scpp 模块源码集合
- `[additional_objs.std-native]` / `[additional_objs.scpp-native]` 各自执行一次
  `${CXX:-c++} -c ...`，产出原生 `.o`
- `additional_objs = "..."` 把这些输出接到最终的 `libstd.scppa` /
  `libscpp.scppa` 归档里

留在 manifest 之外的，是顶层 `../CMakeLists.txt` 里的
`add_custom_target(scpp_workspace_artifacts ...)`：直接 `cd` 到
`${CMAKE_SOURCE_DIR}`（仓库真正的顶层源码目录，真正的 `scpp.toml` 就在
那里）执行刚构建出来的 `scpp build`（不带 `--lib`），再把 `std`/`scpp`
产物复制回顶层其余构建逻辑已经消费的稳定路径——和贡献者自己在仓库根
目录手动执行的命令完全一样。`.scpp/build/`（本来就已经在
`.gitignore` 里）会直接写进源码树，是同一个基于同一份 checkout 搭建的
每一个 CMake build 目录共享的缓存，而不是各自隔离——这是显式接受的
代价，换来的是这一步骤和贡献者自己的工作流完全一致。由于 workspace
没有设置 `default-members`，这一步的 `scpp build` 会真正编译 `src` 的
`ast.cppm` 和 `applications/httpserver` 的 `[[bin]]`，和一直以来对待
`std`/`scpp` 的方式完全一样。这意味着 `ast.cppm` 一旦出现 self-hosting
回归，或者 `httpserver` 构建失败，这一 CMake 步骤（进而整个
`cmake --build`）就会失败，和真正的 stdlib 构建失败一样——这是有意为
之，让两者都变成 CMake 构建本身持续执行的检查，而不是单独运行的检查。
`src` 自己的构建产物（`scpp.ast.scppm`/`libscpp-compiler.scppa`）目前
不会像 `std`/`scpp` 的产物那样被复制到稳定路径，因为这次构建里还没有
别的地方需要消费它们；`applications/httpserver` 构建出的可执行文件则
会被它自己的 `httpserver_app_artifact` 目标复制到稳定路径
（`build/applications/httpserver/httpserver_app_bin`）——见
`../applications/httpserver/CMakeLists.txt`。

`libs/CMakeLists.txt` 又重新出现了，但用途完全不同：它声明了一个
opt-in（`EXCLUDE_FROM_ALL`）的 `scpp_stdlib_cxx_validation` 目标，把
`libs/std/**/*.scpp` 和 `libs/scpp/**/*.scpp` 下的每个文件都当作真正的
C++ 编译一遍，用来验证"每一个合法的 scpp 文件也是合法的 C++"这条语言
不变式——完整说明和当前发现见该文件自己的头部注释。它和上面描述的
manifest/workspace 构建没有任何关系。

## 如何使用 `std`

用户代码写：

```cpp
import std;
```

构建时显式传入模块映射，例如：

```sh
scpp app.scpp -o app \
  --import std=libs/std/std.scpp \
  --import scpp=libs/scpp/scpp.scpp
```

说明：

- `libs/std/std.scpp` 负责聚合 `std` 的各个分区；源码消费者不应直接在代码里写
  `import std:string;` 或 `import std:memory;`
- `libs/scpp/scpp.scpp` 负责聚合 scpp 自己的扩展分区；只有显式
  `import scpp;` 才会使用它们
- 原生辅助对象已经打包进随工具分发的 `libstd.scppa` / `libscpp.scppa`，
  普通使用者不再需要单独传 wrapper `--link`
- 各分区会和主接口单元一起编译成同一个 `std` 模块目标文件，不存在文本拼接

## 当前分区

### `std:string`

- 文件：`std/string/std_string.scpp`
- 底层实现：`std/string/scpp_string_wrapper.{h,cpp}`
- 通过 `extern "C"` 包装函数提供一小部分 `std::string` 能力

### `std:memory`

- 文件：`std/memory/std_memory.scpp`
- 纯 scpp 实现
- 提供 `std::unique_ptr<T>` 和 `std::make_unique<T>(...)`

## 测试原则

`libs/` 是库源码目录，不是演示程序目录。行为覆盖应该进入真正的测试套件：

- `tests/`：dev-agent 负责的单元/集成测试
- `blackbox_test/`：面向用户可见行为的黑盒测试

原生辅助库仍然在这里构建，因为编译器和测试都需要它们；但演示可执行程序
不应继续放在这里。
