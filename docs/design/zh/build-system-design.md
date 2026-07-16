# SCPP project-scale build system design / SCPP 项目级构建系统设计

状态：研究 + 设计草案，供实现前评审

## 0. 范围与核心结论

本文档提出 scpp 应如何从今天的单文件 / 单模块 CLI，演进为适用于真实多包、多模块代码库的**项目级构建系统**。

核心建议是：

1. 将新的**零配置目录约定**保留为最简单场景；
2. 对超出该简单场景的情况，增加一个**可选清单文件**（`scpp.toml`）；
3. 让公开 UX 保持**类似 Cargo 且一体化**（`scpp build`、`scpp run`、`scpp package`、工作区、profile、依赖声明）；
4. 让内部模型保持**artifact-first（制品优先）**，围绕 scpp 现有的 `.scppm` / `.scppa` 分层来构建，而不是围绕类似 CMake、需要手工维护的 target graph。

简言之：**Cargo 风格的用户体验，CMake 级别的项目规模，scpp 原生的制品语义。**

## 1. scpp 当前的起点

今天的参考状态很重要，因为构建系统设计应当扩展它，而不是与它对抗。

### 1.1 现有语言 / 包层面的事实

根据现有的模块 / 库章节和二进制格式规范：

- scpp 已经把**模块**定义为编译 / 导入单元，使用真实的 C++20 风格 `export module`、`module`、`import` 和 partition。[^modules-book]
- `.scppm` 已经存在，作为编译后的**模块接口**格式：每个模块一个文件，携带声明和 generics block，但**不包含机器码，也不包含依赖元数据**。[^scppm-spec]
- `.scppa` 已经存在，作为针对单个模块和单个 target triple 的编译后**原生机器码 archive**。[^modules-book]
- `.scppkg` 已经存在，作为已规定的**分发包**格式，打包一个或多个模块，以及依赖元数据和按模块记录的原生链接需求。[^scppkg-spec]
- 当前语言规范明确指出，**包管理 / 依赖解析 / registry** 属于工具层面的关注点，而不是语言语法层面的关注点。[^modules-out-of-scope]

这是一个很强的起点：scpp 已经具备适合可扩展构建的正确制品边界。

### 1.2 现有 CLI 层面的事实

当前 CLI 已经有两个有用层次：

- `scpp <file.scpp> ...` 直接把单个文件构建成可执行文件。[^cli-current]
- `scpp build-module <file.scpp> --interface-out X.scppm --archive-out Y.scppa` 显式构建单个模块的编译制品。[^cli-current]

这意味着项目构建**不需要**发明新的底层编译器原语。它主要需要的是一个位于这些已合理存在原语之上的 orchestration layer（编排层）。

### 1.3 现有零配置项目模式的先例

暂停中的 `dev-agent/project-build-mode` 分支同样是重要背景：

- 不带参数的裸 `scpp` 会在当前目录查找 `main.scpp` 和 / 或 `lib.scpp`；
- `main.scpp` 构建出一个以清洗后的目录名命名的可执行文件；
- `lib.scpp` 构建 `<name>.scppm` + `lib<name>.scppa`，并要求模块名与清洗后的目录名匹配。[^project-mode-branch]

这是一个不错的**小项目约定**，但它本身无法表达：

- 路径依赖，
- 多包仓库，
- profile，
- 原生库需求，
- 多个二进制目标，
- 可发布的包身份。

因此，核心设计问题不是约定是否有用，而是约定是否足够。我认为答案是：**对大型项目不够，对最简单场景足够**。

## 2. 来自成熟系统的研究结论

## 2.1 Cargo：最相关的高层先例

Cargo 的整体形态，是 scpp 公开 UX 最强的先例。

根据官方 Cargo 文档验证：

- 每个 package 都有一个 `Cargo.toml` manifest；manifest 是 Cargo 发现并编译的单元。[^cargo-manifest]
- 依赖声明清晰地区分 **path**、**git** 和带版本的 registry 依赖。[^cargo-deps]
- workspace 提供共享根目录、共享 lockfile、共享输出目录，以及工作区级别的依赖 / profile 继承。[^cargo-workspaces]
- profile 是一等概念（`dev`、`release`、自定义 profile），而且 Cargo 按命令选择默认值（`cargo build` => `dev`，`--release` => `release`）。[^cargo-profiles]
- `Cargo.lock` 记录精确解析后的依赖，并由 Cargo 维护，而不是手工编辑。[^cargo-lock]
- Cargo 支持**构建脚本**（`build.rs`）和 **feature**，但两者之所以存在，是因为 Rust 拥有成熟的宿主执行模型和语言级条件编译系统。[^cargo-build-scripts][^cargo-features]

我还在本地验证了一个最小 Cargo workspace：

```console
$ cargo build --workspace
   Compiling lib v0.1.0 (.../lib)
   Compiling app v0.1.0 (.../app)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.43s
```

并且 Cargo 创建了单一的 workspace-root `Cargo.lock` 和共享的 `target/` 目录。[^local-cargo-check]

### Cargo 对 scpp 的启示

保留：

- 每个 package 一个 manifest；
- 带共享输出 / 缓存的 workspace root；
- 直接依赖声明；
- 显式 profile；
- 以 package 为中心而不是以文件为中心的思维方式。

**不要**直接照搬：

- v1 中 `build.rs` 风格的任意宿主代码；
- Cargo 的 feature / 条件依赖激活，因为 scpp 有意不提供预处理器，也没有现成的条件编译设计。[^modules-book]

## 2.2 CMake：组合能力和传递性原生依赖需求的最佳先例

根据官方 CMake 文档验证：

- CMake 明确是一个**跨平台构建系统生成器**：它生成原生构建系统，然后由 CMake 或原生构建工具执行它。[^cmake-manual]
- `add_subdirectory()` 组合按目录划分的构建描述，并立即处理子目录中的 `CMakeLists.txt`。[^cmake-add-subdirectory]
- `target_link_libraries()` 会以传递方式传播**使用需求**（usage requirements）；被链接的库会同时影响下游 target 的编译和链接。[^cmake-target-link]
- `find_package()` 的存在正是为了发现外部提供的 package，并把导入的 target / 配置暴露到本地构建图中。[^cmake-find-package]
- CMake Presets 提供结构化的 configure / build / test 工作流和命名配置，无需用户记忆冗长命令行。[^cmake-presets]

我还在本地验证了两阶段的 CMake 工作流：

```console
$ cmake -S . -B build
-- Configuring done
-- Generating done
-- Build files have been written to: .../build

$ cmake --build build
[100%] Built target app
```

这很重要，因为它非常具体地展示了核心的 generator split（生成器分层）。[^local-cmake-check]

### CMake 对 scpp 的启示

保留：

- 面向大型仓库的显式组合；
- 原生依赖的传递性传播；
- “usage requirements（使用需求）是第一类构建元数据”这一思想。

**不要**把它直接照搬成主要用户模型：

- 独立的构建语言 / generator-first 工作流。

为什么不行？因为 CMake 的 raison d'etre（存在理由）是跨语言、跨工具链、跨 IDE 生成。scpp 的项目构建系统更窄：它主要关注**构建 scpp package，而这些 package 的语义本就围绕 `.scppm`、`.scppa`、模块导入和包元数据展开**。

## 2.3 Bazel / bzlmod：严格依赖可见性与可锁定的外部解析

根据 Bazel 官方外部依赖文档验证：

- Bazel 的现代外部系统基于 `MODULE.bazel` 和带版本的模块。[^bazel-overview]
- Bazel 会在获取 / 构建之前解析完整的传递图，并强调确定性的版本解析、严格的依赖可见性，以及 lockfile / vendor mode。[^bazel-overview]

### Bazel 对 scpp 的启示

Bazel 对 scpp 最有价值的思想是**严格的直接依赖可见性**：

- 如果 package `A` 依赖 package `B`，而 `B` 依赖 `C`，那么 package `A` 不应隐式获得对 `C` 模块的编译期可见性。
- 传递依赖对最终链接仍然重要，但直接声明应保持为编译期契约。

这条规则会让 scpp 在项目级场景中的行为更可预测，也同时符合 Bazel 和 Cargo 的直觉。

## 2.4 Go modules：去中心化身份与简单的包根 manifest

根据官方 Go module 文档验证：

- 模块通过声明在模块根中的路径 `go.mod` 来标识。[^go-mod-ref]
- Go 使用模块级依赖管理，而不是按文件记录依赖元数据。[^go-managing-deps]
- `go.mod` 和 `go.sum` 位于模块根，并且预期应提交到版本控制中。[^go-managing-deps]

### Go 对 scpp 的启示

有用的启示不是 Go 的具体文件格式，而是它的**身份模型**：

- package identity 应驻留在 package root；
- package 内的模块名可以比 package identity 更细粒度；
- 外部分发的 package 日后仍可保持去中心化，而无需改变本地构建模型。

这很适合 scpp，因为 scpp 已经把包元数据（`.scppkg`）与模块名（`mylib.math`、`org.example.net` 等）区分开来。

## 2.5 Zig 构建系统：有价值，但不是 scpp 的正确 v1 形态

根据 Zig 官方文档验证：

- Zig 将 `build.zig` 定位为在命令行过长、出现依赖，或需要并发 / 缓存 / 配置时额外提供的一层抽象。[^zig-build]
- Zig 的构建图是显式的 DAG，并内建缓存和并发。[^zig-build]

### Zig 对 scpp 的启示

有吸引力的部分是 DAG / 缓存思维。

对 scpp v1 不吸引人的部分是**“构建系统就是用这门语言写的用户代码”**：

- scpp 还不是自举（self-hosting）的；
- scpp 还没有适合构建脚本的成熟稳定标准库 / 运行时；
- 在 configure 阶段求值项目提供的任意 scpp 代码，会带来很大的安全性、确定性和自举问题；
- 用户要的是适用于真实构建、类似 CMake / Cargo 的东西，而不是第二个通用执行环境。

因此：吸收 Zig 关于图和缓存的雄心，但**不要**采用 Zig 那种“构建配置就是程序”的模型。

## 2.6 Meson：适合原生依赖的良好声明式人体工学

根据 Meson 的依赖文档验证：

- `dependency('zlib')` 在 pkg-config / CMake / system discovery 之上提供了一层声明式、对用户友好的封装；下游 target 消费的是 dependency 对象，而不是手工 flags。[^meson-deps]
- Meson 也支持 system-fallback / subproject 模式，以应对依赖在宿主系统中缺失的情况。[^meson-deps]

### Meson 对 scpp 的启示

Meson 最好的启示在于**原生 / 外部依赖声明**：

- scpp 应尽可能优先使用声明式 manifest 形式，而不是裸 linker flags；
- 但 v1 可以先从更简单的手工声明模型起步，把更丰富的发现提供者（`pkg-config`、`cmake` 等）留作后续叠加层。

## 3. scpp 的设计原则

基于上述研究和 scpp 当前架构，我认为构建系统应遵循以下规则。

1. **以 package 为中心，而不是以文件为中心。**
   单文件 `scpp foo.scpp` 依然有效，但大型项目构建必须以 package / workspace / profile 为思考单位。

2. **基于 manifest 的项目模式；无 manifest 的非项目模式。**
   零配置的单文件用法继续有效，但 package / workspace 构建应基于 manifest。

3. **公开 UX 应一体化，而不是 generator-first。**
   用户应运行 `scpp build`，而不是 `scpp gen && ninja`。

4. **制品是构建图的边界。**
   `.scppm` 是编译期依赖边界；`.scppa` 是链接期制品边界；`.scppkg` 是分发边界。

5. **直接依赖是编译期契约。**
   传递 package 可以被传递链接，但可导入模块应只来自当前 package 或直接依赖。

6. **v1 中不提供可编程构建脚本。**
   只允许声明式 manifest。

7. **在语言本身尚未形成连贯方案之前，不要在构建系统里发明 feature / 条件编译。**

## 4. 推荐的顶层设计

## 4.1 公开模型

引入一个名为 `scpp.toml` 的**可选 manifest**。

- 如果存在 `scpp.toml`，则该目录是一个基于 manifest 的 package 或 workspace。
- 如果不存在 `scpp.toml`，则用户处于**非项目模式**。

这样既能让项目级行为保持显式，又不会打扰 scpp 现有那种“直接当编译器用”的非项目使用体验。

## 4.2 执行模型

暴露一组**类似 Cargo 的一体化命令集**：

- `scpp build`
- `scpp run`
- `scpp test`
- `scpp package`
- `scpp clean`

并让裸 `scpp` 作为“构建当前目录默认目标”的便捷别名。

## 4.3 内部引擎选择

**决策：v1 使用一体化 planner / executor，而不是强制性的 Ninja / CMake 风格 generator。**

原因：

- scpp 的构建图是 scpp 特有的：模块、partition、`.scppm`、`.scppa`、generics block、package manifest；
- 强制依赖 Ninja 或其他后端会让第一版用户体验更复杂；
- Cargo 证明了一体化工具足以扩展到大型真实项目；
- 现有的制品分层已经给 scpp 带来了 Cargo 所没有的天然增量边界。

v1 **不需要**后端抽象层，也不需要生成式后端的逃生口。如果这种需求以后真的出现，可以再把它作为单独命令（`scpp gen --backend ...`）加入，而不是现在就让 v1 架构围绕一个用户并不想立即要求的能力来塑形。

## 5. 项目 / 包模型

## 5.1 package 与 module 的区别

构建系统应作出与现有 `.scppkg` 格式一致的区分：

- **package** 是依赖声明和 workspace 成员资格的单位；
- **module** 是 import / export 与 `.scppm` 生成的单位。

一个 package 可以提供：

- 零个或一个 library target；
- 零个或多个 binary target；
- 这些 target 内的一个或多个模块。

这既符合 Cargo / Go 的 package 思维，也符合 scpp 现有的包格式。

## 5.2 无 manifest 的非项目模式

`scpp.toml` 的缺失应被理解为**非项目模式**，而不是另一种完整的项目模型。

今天的非项目模式，就是现有的单文件 CLI 行为：

- `scpp <file.scpp> ...` 直接编译单个文件。[^cli-current]

暂停中的 `main.scpp` / `lib.scpp` 分支，依然可作为 scpp 曾探索过的“无 manifest 的目录级便利”最接近的背景，但本设计**不会**把它扩展成 v1 主项目故事的一部分，也**不会**加入 `src/main.scpp` / `src/lib.scpp` 识别。即使将来保留 `main.scpp` / `lib.scpp` 的便利性，它也应被文档化为一种狭窄的非项目便利，而不是通用项目模式。[^project-mode-branch]

scpp 将来是否会增长出一种更严格的多文件无 manifest 模式，被明确推迟处理；当前 package / workspace 构建仍以 manifest 为基础。

## 5.3 基于 manifest 的模式

推荐的 manifest 文件：`scpp.toml`

示例：

```toml
manifest-version = 1

[package]
name = "httpserver"
version = "0.1.0"          # required for packaging, recommended otherwise

[[lib]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[[bin]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[dependencies]
net = { path = "../net" }
json = { scppkg = "vendor/json.scppkg" }

[profile.dev]
opt-level = 0
debug = true
static = false

[profile.release]
opt-level = 3
debug = false
static = true

[native]
links = ["pthread"]
search = ["native/lib"]
```

### 为什么选 TOML？

因为 Cargo 已经证明，TOML 既足够易读、适合手工编辑，也足够结构化、适合工具处理，而且比 XML 或新 DSL 的样板更少。[^cargo-manifest]

### 为什么不是 `CMakeLists.txt` 风格的 DSL？

因为 scpp 不需要第二门语言来承载项目元数据。声明式 table 已经足够覆盖真实的 v1 需求。

## 5.4 manifest 字段

推荐的初始字段：

- `manifest-version` —— manifest 格式的 schema 版本。
- `[package]`
  - `name`（必需）
  - `version`（`scpp package` 必需，其余情况可选但推荐）
- `[[lib]]`
  - `name`
  - `sources`
  - 可选 `additional_objs`
- `[[bin]]`
  - `name`
  - `sources`
  - 可选 `additional_objs`
- `[dependencies]`
- `[profile.dev]`、`[profile.release]`、可选的自定义 `[profile.<name>]`
- `[native]`，用于 package 级原生链接需求
- `[additional_objs.<name>]`，用于可被 target 引用、能产出额外对象文件的自定义构建步骤
- `[package.metadata]`，预留给外部工具

推荐的 v1 限制：**每个 package 最多一个 library target**。

如果一个仓库需要很多可独立复用的库，它应使用包含多个 package 的 workspace，而不是在一个 package 里放多个 library target。这样能让打包和依赖解析保持更简单。

## 6. workspace 模型

采用类似 Cargo 的 workspace root。

根 manifest 示例：

```toml
manifest-version = 1

[workspace]
members = [
  "libs/net",
  "libs/json",
  "apps/httpserver",
]
default-members = ["apps/httpserver"]

[workspace.dependencies]
net = { path = "libs/net" }
json = { path = "libs/json" }

[profile.dev]
opt-level = 0
debug = true

[profile.release]
opt-level = 3
debug = false
```

成员 manifest：

```toml
manifest-version = 1

[package]
name = "httpserver"
version = "0.1.0"

[[bin]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[dependencies]
net = { workspace = true }
json = { workspace = true }
```

### workspace 规则

推荐规则：

- workspace root 可以是以下两种之一：
  - **virtual workspace**（只有 `[workspace]`），或
  - **root package workspace**（`[workspace]` + `[package]`），与 Cargo 保持一致。[^cargo-workspaces]
- 在 workspace root 运行 `scpp build` 会构建 `default-members`；`scpp build --workspace` 会构建全部成员。
- 定义在 workspace root 的 profile 会覆盖成员本地 profile table，与 Cargo 的“profile 归根所有”模型保持一致。[^cargo-workspaces][^cargo-profiles]
- workspace root 拥有共享的 build / cache 输出目录。

### 为什么用 workspace，而不是 CMake 的 `add_subdirectory()`？

因为 workspace 建模的是**具有身份的 package**，而不仅仅是带命令式脚本包含关系的目录。这更契合 scpp 未来的 `.scppkg` 生态。

## 7. 依赖模型

## 7.1 v1 依赖种类

推荐的 v1 依赖来源：

1. `path = "../foo"`
   - 一个包含 `scpp.toml` 的本地 package 目录
2. `scppkg = "vendor/foo.scppkg"`
   - 一个打包好的依赖文件

预留但暂不实现：

- `git = ...`
- 带 registry 源的 `version = ...`

这样可以在不把设计逼进死角的同时，让 v1 与今天实际存在的能力保持对齐。

## 7.2 编译期可见性规则

**建议：模块解析只允许看到直接依赖。**

如果 package `app` 依赖 package `net`，而 `net` 依赖 `tls`，那么：

- `app` 可以 import 由 `net` 导出的模块；
- 除非 `app` 的 `[dependencies]` 里也直接声明了 `tls`，否则 `app` **不能** import 仅由 `tls` 导出的模块。

原因：

- 符合 Cargo / Bazel 的预期；
- 让 package manifest 保持诚实；
- 防止对传递 package 结构形成隐藏耦合。

## 7.3 链接期闭包规则

编译期可见性仅限直接依赖，但最终链接仍应使用**传递依赖闭包**，与现有 scpp 语言规范中的链接模型保持一致。[^modules-linking]

这意味着：

- 被 import 的模块必须通过直接依赖解析；
- 被链接的 `.scppa` archive 和原生链接需求会进行传递传播。

这正是 CMake 的传递性 usage requirements 思想最有价值的地方。[^cmake-target-link]

## 7.4 未来的 lockfile

我建议**现在就**确定文件名：`scpp.lock`。

不过，我**不**建议把它做成 v1 的核心特性。

理由：

- 一旦存在 registry / git 依赖，Cargo 的 lockfile 就非常有价值。[^cargo-lock]
- 在只有 path 的生态中，lockfile 无法从强意义上保证可复现性，因为本地路径是可变的；
- 过早强推沉重的 lock 语义，只会增加改动噪音，却带不来等价价值。

因此，推荐路径是：

- 从第一天起就预留 `scpp.lock`；
- 为未来的依赖解析 / 抓取步骤预留位置，最合适的公开形式是 `scpp pull`，等到 registry / git 依赖真正存在时再启用它；
- 允许 `scpp package` / 未来 registry 工作在之后填充和消费它；
- 让 v1 的本地 workspace 构建在不依赖它的情况下也能正常工作。

## 8. 源发现与 target graph 构建

## 8.1 manifest 声明的源码集合

每个 target（`[[lib]]`、`[[bin]]`）都应声明：

- 一个 `name` 字段（`[[lib]]` 的产物名或 `[[bin]]` 的可执行文件名）；
- 一组 `sources` glob。
- 可选的 `additional_objs` 引用，用来点名一个或多个 `[additional_objs.<name>]` 步骤。

每个 `[additional_objs.<name>]` block 声明：

- `input` —— 参与增量判断的输入文件；
- `output` —— 该命令必须产出的文件；
- `command` —— 只执行一次的 shell 命令；它产出的对象文件会被送入最终的归档 / 链接步骤。

然后构建工具解析列出的源文件，以发现：

- 主接口单元（`export module foo;`）
- 实现单元（`module foo;`）
- 接口 partition（`export module foo:bar;`）
- 实现 partition（`module foo:bar;`）
- 普通非模块源文件。

这样可以避免强迫用户在 manifest 中手工重复模块图。

## 8.2 分组规则

在同一个 target 的源码集合内：

- 所有声明相同模块名的单元都属于同一个逻辑模块构建；
- 每个模块必须且只能有一个主接口单元；
- 实现单元和 partition 挂接到该模块；
- 普通非模块文件成为所属 target 的常规编译单元。

这只是现有第 11 章规则在项目构建层面的推广。[^modules-book]

## 8.3 跨 package 的模块解析

在规划期间，构建工具会构造一张表：

- 模块名 -> 产出该模块的 package + 产出该模块的 target + 预期 `.scppm` 路径

解析顺序：

1. 当前 target / package 的模块
2. 直接依赖 package 导出的模块
3. stdlib 内建模块 / 随工具分发的 package root
4. 如果 CLI 以后为项目模式加入显式 override，则最后处理这些 override

歧义应当是硬错误：

- 如果两个直接依赖都导出了同一个模块名，用户必须通过修改依赖 / package 选择来消除歧义，而不是在 import 点做别名处理。

这与 scpp 当前避免隐藏查找魔法的语言偏好保持一致。

## 9. 制品模型以及与 `.scppm` / `.scppa` / `.scppkg` 的集成

## 9.1 关键观察

scpp 现有的制品分层，本身就已经是正确的增量边界：

- `.scppm` 变化意味着**编译期接口变化**；
- `.scppa` 变化而 `.scppm` 不变，意味着**需要重新链接，但可能可以避免下游重新编译**。

这是相对于 header 风格系统的一个重大优势。

## 9.2 本地构建输出

推荐的 workspace 本地输出根目录：

```text
.scpp/
  build/
    <triple>/
      <profile>/
        <package>/
          modules/
            foo.scppm
            bar.scppm
          archives/
            libfoo.scppa
            libbar.scppa
          objects/
          package-metadata.json
  cache/
    build.db
```

说明：

- `.scpp/` 有意只用于项目本地，不用于分发。
- `package-metadata.json` 是内部构建制品，不是公开发布格式。
- 内部元数据的形状应设计得能干净地映射到 `.scppkg` 的 `MANIFEST.json`，这样本地依赖与打包依赖就共享同一个概念模型。

## 9.3 package 构建输出

library package 的构建应产出：

- 每个导出模块一个 `.scppm`；
- 每个已编译模块、每个 target triple 一个 `.scppa`；
- 将 package -> 导出模块 -> 制品路径 -> 原生链接需求进行映射的元数据。

binary package 的构建应产出：

- 普通源文件和模块本地编译代码对应的常规 object file；
- 最终链接好的可执行文件。

## 9.4 打包命令

引入：

```console
scpp package
```

行为：

- 仅对带 `[[lib]]` 的 package 有效；
- 在所选 profile / target 下构建该 package；
- 按照现有规范，将生成的 `.scppm` / `.scppa` 制品和 manifest 元数据一起打包进 `.scppkg` 文件。[^scppkg-spec]

建议的默认输出位置：

```text
dist/<package-name>-<version>-<target-triple>.scppkg
```

`.scppkg` 规范已经支持每个模块包含多个 target triple；v1 打包可以先从每次调用一个 triple 开始，后续再扩展。

## 9.5 打包输出中的原生链接需求

`.scppkg` 规范已经按模块记录 `native_link_requirements`。[^scppkg-spec]

对于 v1，target 级 / package 级原生需求可以保守地 lower 到该 schema 中：

- 如果某个 library target 声明了 package 级 `links = ["ssl", "crypto"]`，那么从该 target 发出的每个二进制模块都可以在生成的 package manifest 中携带同样的需求集合。

这会略显粗粒度，但它是正确的，以后若有需要可以再细化为按模块声明。

## 10. profile 与配置

## 10.1 内建 profile

采用内建 profile：

- `dev`
- `release`

并允许可选的自定义 profile，与 Cargo 保持一致。[^cargo-profiles]

推荐默认值：

### `dev`

- `opt-level = 0`
- `debug = true`
- `static = false`
- 启用增量重建

### `release`

- `opt-level = 3`
- `debug = false`
- 默认 `static = false`，可按项目覆盖
- 以后再加入更激进的 codegen / LTO hook

## 10.2 CLI / profile 交互

推荐命令：

- `scpp build` => `dev`
- `scpp build --release` => `release`
- `scpp build --profile release-lto` => 自定义命名 profile
- `scpp run` => 默认用 `dev` 构建，除非被覆盖

现有的一次性 flag 应能自然映射：

- 现有单文件 `-g` 概念映射到 `debug = true`
- 现有 `--static` 概念映射到 `static = true`

对于项目模式，这些应当**先是 profile 属性，其次才是 CLI override**。

## 10.3 为什么 v1 不提供 Cargo 风格的 feature？

因为 Cargo feature 与 Rust 的条件编译以及可选依赖激活紧密绑定。[^cargo-features]

scpp 明确没有预处理器，而语言目前也没有定稿的条件编译 feature 体系。[^modules-book]

因此，现在就加入构建系统 feature 矩阵，只会给尚不存在的隐藏语言语义制造压力。

## 11. 原生 / 外部依赖方案

## 11.1 v1 建议

v1 应在 manifest 中支持一种**声明式、手工维护**的原生依赖模型。

推荐字段：

```toml
[native]
links = ["m", "pthread"]
search = ["native/lib", "/opt/foo/lib"]
```

后续可扩展字段：

```toml
frameworks = ["Security"]
discover = ["openssl"]
```

## 11.2 为什么先从手工模型开始？

因为它已经足以把今天原始的 `--link path` 世界，替换成一种适用于项目规模、可纳入版本控制的模型，而无需立刻迫使 scpp 变成一个跨平台系统包发现框架。

Meson 的例子仍然有价值，因为它说明了：**声明式依赖描述优于原始 flag 拼盘**。[^meson-deps] 但用户评审也澄清了一个 scpp 特有的约束：未来的演进方向应是**scpp 原生的发现机制**，而不是长期依赖 `pkg-config`、CMake package discovery 或其他第三方发现工具。

## 11.3 传播规则

原生链接需求应像 CMake 的 `PUBLIC` usage requirements 一样进行传递传播：[^cmake-target-link]

- 如果 package `net` 依赖原生 `ssl` 和 `crypto`，那么任何依赖 `net` 的最终可执行文件都应自动继承这些链接需求。

## 12. CLI 表层

## 12.1 保留现有底层命令

保留：

- `scpp <file.scpp> ...`
- `scpp build-module ...`
- `scpp lex ...`
- `scpp parse ...`

`build-module` 继续充当单模块制品构建的专家 / plumbing 命令。

## 12.2 增加项目级命令

推荐新增命令：

```console
scpp build [--workspace] [-p <package>] [--bin <name>] [--lib] [--profile <name>] [--release]
scpp run   [--workspace] [-p <package>] [--bin <name>] [--profile <name>] [--release] [-- <program args>]
scpp test  [--workspace] [-p <package>] [--profile <name>] [--release] [-- <test args>]
scpp package [-p <package>] [--profile <name>] [--release]
scpp clean
```

用于选择 package 的 flag 有意与 Cargo 保持一致，因为它们已被证明适用于多 package 仓库，而且不会让人意外。[^cargo-workspaces]

`scpp test` 应当现在就作为一等项目命令被命名下来，但它的实际机制**还无法**被完整设计，因为 scpp 目前还没有类似 Rust `#[test]` + `cargo test` 那样的语言级测试函数 / attribute / 发现模型。换句话说，这个命令名应当现在就进入构建系统路线图，而它未来究竟如何执行，很可能要依赖后续的语言 / 工具设计来定义项目级测试 target（例如 manifest 声明的 test target，以及 / 或在语言具备一致的“可运行测试”标记方式后，再约定 `tests/` 目录布局）。

另外，为其他后端生成构建文件的未来逃生口，应当表现为一个显式子命令：

```console
scpp gen --backend ninja
```

而不是仅仅作为挂在 `scpp build` 下面的一个 flag。用户评审也进一步澄清：`scpp gen` **不是** v1 需求，它只是后续才加入的能力。

## 12.3 裸 `scpp`

推荐行为：

- 如果给定 `.scpp` 文件参数 => 保持当前单文件行为
- 如果没有给定文件参数，且检测到 project / workspace => 表现为 `scpp build`，并构建默认 target
- 如果没有给定文件参数，且未检测到 project => 进入当前 help / error 路径

这样既保留了本次会话里“零样板”的方向，也不会让项目模式产生歧义。

另外，现在就把这个选择说清楚，还有一个面向未来的理由：一旦 scpp 将来真正获得 registry / git 依赖解析能力，裸 `scpp` 就可以很自然地被理解为 **`scpp pull` + `scpp build`** —— 先解析 / 抓取外部依赖，再执行构建。由于今天还没有这样的 pull / registry 机制，裸 `scpp` 现在就应当简单地等价于 `scpp build`。这样既保持了当前 UX 的简洁，也为未来的外部依赖工作流留下了清晰的概念路径。

这条规则只在检测到**基于 manifest 的 project / workspace** 时适用。若不存在 manifest，则仍按 §5.2 所述进入非项目模式。

## 13. 增量与并行构建策略

## 13.1 为什么 scpp 的制品分层让这件事可行

构建系统可以利用两个不同的失效边界：

- **编译失效**：任意上游 `.scppm` 发生变化
- **链接失效**：任意上游 `.scppa` 发生变化，即使 `.scppm` 没变

这意味着：

- 只改动依赖模块中的实现代码，通常可以避免下游重新编译；
- 下游二进制仍会重新链接新的 `.scppa`。

这是该设计最强的收益之一，应被显式利用。

## 13.2 构建数据库

推荐的内部状态：

- 一个本地 SQLite 构建数据库（例如 `.scpp/cache/build.db`）
- 每个已编译模块 / 普通源文件 / binary target / package 制品一行
- 缓存键包括：
  - 源文件摘要，或 mtime + size
  - 活跃 profile
  - target triple
  - 编译器版本
  - 已解析依赖制品的摘要 / 路径
  - manifest 摘要

## 13.3 调度器

推荐的 v1 调度器行为：

- 扫描 manifest 和源文件；
- 构造 package graph；
- 在每个 package 内部构造 module / import DAG；
- 并行调度已就绪的模块；
- 在所需的本地 / 依赖 `.scppm` 制品存在后，再调度普通源文件编译；
- 当所有 object / archive 输入准备好后再链接 target。

这本质上就是 Cargo 的一体化执行模型，只不过使用了 scpp 特有的模块 / 接口边界。

## 13.4 为什么 v1 不强制生成 Ninja？

因为对 scpp 而言，困难之处在于**图语义**，而不在于 shelling-out 机制。

一旦 scpp 已经知道：

- 存在哪些模块，
- 哪些 package 导出它们，
- 哪些 `.scppm` 变化需要重新编译，
- 哪些 `.scppa` 变化需要重新链接，

那么直接执行这张图，就是一个合理的第一版实现。

如果后来证明它对 IDE、调试或外部构建工具集成有帮助，再加入类似 `scpp gen --backend ninja` 的逃生口即可；但在那种真实需求出现之前，v1 不应背上后端抽象的设计负担。

## 14. 考虑过的替代方案

## 14.1 纯文件系统约定，永远不要 manifest

作为主方案，拒绝。

原因：

- 无法干净地表达依赖 / workspace / profile / 原生链接元数据；
- 无论如何都无法在没有额外文件的前提下支持可发布的 package identity；
- 随着项目规模扩大，只会被迫引入越来越“魔法”的启发式规则。

仅把它保留为简单模式的后备方案。

## 14.2 每个项目都强制要求 manifest，即使是极小项目

拒绝。

原因：

- 这与当前 `scpp foo.scpp` / 零 flag 方向相冲突；
- 会让最简单场景为了很小收益而变得更糟。

## 14.3 把 CMake 风格 generator 作为主模型

作为 v1 公开 UX，拒绝。

原因：

- 对一个已经拥有编译语义控制权的单语言工具链来说，样板过多；
- 会让 scpp 项目构建看起来像“为另一个构建工具写构建脚本”，而不是“用 scpp 构建 scpp”；
- generator 导出适合作为后续可选特性，而不是核心心智模型。

## 14.4 构建系统就是 scpp 代码（`build.scpp`，Zig 风格）

作为 v1，拒绝。

原因：

- 自举与宿主执行复杂度；
- 安全性 / 确定性顾虑；
- 相对于生态当前所需能力来说，威力过剩。

## 15. 推荐的分阶段实现计划

## Phase A —— 落地项目模型骨架

1. 增加项目 / workspace 发现（向上搜索 `scpp.toml`）；
2. 让 manifest 的存在成为 project mode 与 non-project mode 的分界线；
3. 增加 `scpp build` 作为显式的项目级命令；
4. 让 project / workspace 模式下的裸 `scpp` 表现为 `scpp build`。

## Phase B —— manifest 解析器 + 单 package 构建

1. 解析 `scpp.toml`
2. 支持 `[package]`、`[[lib]]`、`[[bin]]`、`[profile.*]`
3. 构建一个不含外部依赖的 manifest-based package
4. 将本地输出写到 `.scpp/build/...`

## Phase C —— 路径依赖 + workspace

1. 增加 `[dependencies] path = ...`
2. 增加 `[workspace]`、`members`、`default-members`
3. 增加 `-p/--package`、`--workspace`
4. 在 workspace root 共享 build 输出 / cache

## Phase D —— 增量图 + 原生元数据

1. 增加构建数据库
2. 用 `.scppm` 与 `.scppa` 区分编译失效和链接失效
3. 增加声明式 `[native]` 传播
4. 并行化已就绪节点

## Phase E —— 打包 / `.scppkg`

1. 增加 `scpp package`
2. 消费 `scppkg = ...` 依赖
3. 将本地 package 元数据映射到 `.scppkg` 的 `MANIFEST.json`
4. 为未来的非 path 依赖解析预留 `scpp.lock` 语义

## Phase F —— 可选的后续增强

- 在声明式元数据之上增加一种 scpp 原生的原生依赖发现机制
- 当 registry / git 依赖源真正存在后，加入用于解析 / 抓取它们的 `scpp pull`
- 用于生成外部后端构建文件的 `scpp gen --backend <name>`（例如 Ninja）
- 面向 IDE 工具的 `scpp metadata` JSON
- git / version / registry 依赖
- 当 scpp 具备一致的项目测试 / 测试发现语言 / 工具故事后，加入 `scpp test`
- 当生态形成更清晰约定后，再加入 tests / examples / bench target

## 16. 最终敲定的设计决策

用户评审现已解决剩余分叉。最终设计决策如下：

1. **以 `scpp.toml` 作为 package / workspace 项目模式的边界。**
2. **将 manifest 缺失视为非项目模式；今天这意味着单文件使用，而任何保留的 `main.scpp` / `lib.scpp` 便利都应保持狭窄且无 manifest。**
3. **让公开构建 UX 保持一体化、类似 Cargo（`scpp build`、`scpp run`、`scpp package`，以及后续的 `scpp test`），而不是 generator-first。**
4. **当检测到基于 manifest 的 project / workspace 时，让裸 `scpp` 表现为 `scpp build`；未来一旦存在外部解析，这在概念上可以扩展为 `scpp pull` + `scpp build`。**
5. **用 package / workspace 作为依赖模型；用模块作为编译制品模型。**
6. **编译期模块可见性只允许直接依赖。**
7. **最终链接时，以传递方式传播 `.scppa` 和原生链接需求。**
8. **v1 不加入可编程构建脚本。**
9. **v1 不加入 Cargo 风格的 feature / 条件依赖激活。**
10. **现在就预留 `scpp.lock`，但不要让它成为仅 path 的 v1 构建中的核心机制。**
11. **v1 保持原生依赖手工声明，并规划未来的 scpp 原生发现机制，而不是 `pkg-config` / CMake 风格的发现 provider。**
12. **v1 不要求 `scpp gen` 或后端抽象；只有在真实需求出现后，才加入生成式后端支持。**
13. **对 `scpp package` 强制要求 package `version`，但对仅本地构建保持可选。**
14. **利用 `.scppm` 与 `.scppa` 作为彼此独立的增量失效边界。**

## Appendix A —— 值得保留的实证发现

### Cargo

本工作区中执行的本地检查：

```console
$ cargo build --workspace
   Compiling lib v0.1.0 (.../lib)
   Compiling app v0.1.0 (.../app)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.43s
```

观察结果：

- 一个 workspace-root `Cargo.lock`
- 一个共享的 workspace-root `target/`
- 默认构建使用了 Cargo 的 `dev` profile

### CMake

本工作区中执行的本地检查：

```console
$ cmake -S . -B build
-- Configuring done
-- Generating done
-- Build files have been written to: .../build

$ cmake --build build
[100%] Built target app
```

观察结果：

- CMake 的公开模型显然是先 configure / generate，再 build。

### scpp reference repo

reference repo 自己的 stdlib CMake 目前会驱动 `scpp build-module` 产出：

- `std.scppm`
- `libstd.scppa`

这些来自 stdlib 的模块源，而这正是本设计所要推广的、面向制品的项目构建。[^stdlib-cmake]

## Sources

[^modules-book]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md`，尤其是模块 / package / 制品相关章节以及 §11.12-§11.15。
[^scppm-spec]: `scpp-reference/docs/spec/en/scppm-format.md`.
[^scppkg-spec]: `scpp-reference/docs/spec/en/scppkg-format.md`.
[^modules-out-of-scope]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md` §11.15.
[^cli-current]: `scpp-reference/src/cli.cppm:603-691`.
[^project-mode-branch]: `scpp-reference` 远端分支 `origin/dev-agent/project-build-mode`，尤其是 `src/cli.cppm:669-742` 以及通过 `git grep` 找到的相关测试 / 文档。
[^modules-linking]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md:464-522`.
[^stdlib-cmake]: `scpp-reference/stdlib/CMakeLists.txt:14-35`.
[^cargo-manifest]: Cargo Book, "The Manifest Format" — https://doc.rust-lang.org/cargo/reference/manifest.html
[^cargo-deps]: Cargo Book, "Specifying Dependencies" — https://doc.rust-lang.org/cargo/reference/specifying-dependencies.html
[^cargo-workspaces]: Cargo Book, "Workspaces" — https://doc.rust-lang.org/cargo/reference/workspaces.html
[^cargo-profiles]: Cargo Book, "Profiles" — https://doc.rust-lang.org/cargo/reference/profiles.html
[^cargo-lock]: Cargo Book, "Cargo.toml vs Cargo.lock" — https://doc.rust-lang.org/cargo/guide/cargo-toml-vs-cargo-lock.html
[^cargo-build-scripts]: Cargo Book, "Build Scripts" — https://doc.rust-lang.org/cargo/reference/build-scripts.html
[^cargo-features]: Cargo Book, "Features" — https://doc.rust-lang.org/cargo/reference/features.html
[^cmake-manual]: CMake manual `cmake(1)` — https://cmake.org/cmake/help/latest/manual/cmake.1.html
[^cmake-add-subdirectory]: CMake `add_subdirectory()` — https://cmake.org/cmake/help/latest/command/add_subdirectory.html
[^cmake-target-link]: CMake `target_link_libraries()` and `cmake-buildsystem(7)` — https://cmake.org/cmake/help/latest/command/target_link_libraries.html and https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html
[^cmake-find-package]: CMake `find_package()` — https://cmake.org/cmake/help/latest/command/find_package.html
[^cmake-presets]: CMake `cmake-presets(7)` — https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
[^bazel-overview]: Bazel external dependency overview — https://bazel.build/external/overview
[^go-mod-ref]: Go Modules Reference — https://go.dev/ref/mod
[^go-managing-deps]: Go, "Managing dependencies" — https://go.dev/doc/modules/managing-dependencies
[^zig-build]: Zig, "Zig Build System" — https://ziglang.org/learn/build-system/
[^meson-deps]: Meson, "Dependencies" — https://mesonbuild.com/Dependencies.html
[^local-cargo-check]: 本次会话中在 `~/scpp-agents/build-system-designer-agent/scratch-research` 里运行的本地命令。
[^local-cmake-check]: 本次会话中在 `~/scpp-agents/build-system-designer-agent/scratch-cmake` 里运行的本地命令。
