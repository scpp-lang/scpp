# The SCPP Programming Language

- [开始上手](ch01-00-getting-started.md)
  - [构建编译器](ch01-01-building-the-compiler.md)
  - [Hello, World!](ch01-02-hello-world.md)
  - [第一个 project build](ch01-03-hello-project-builds.md)

- [做一个猜数字小游戏](ch02-00-guessing-game.md)

- 常见编程概念
  - [变量与显式初始化](ch03-01-variables-and-explicit-initialization.md)
  - 数据类型
  - 函数
  - 注释
  - 控制流

- 理解所有权
  - 什么是所有权
  - 引用与借用
  - `std::span` 与其它非拥有视图

- 用 struct 与 class 组织相关数据
  - 定义与实例化 `struct` / `class`
  - 一个使用受检查 class 的示例程序
  - 方法与 `this`

- 安全边界与 `[[scpp::unsafe]]`
  - `[[scpp::unsafe]]` 会做什么、不会做什么
  - 调用 `extern "C"` 与使用裸指针
  - 如何把“信任”局部化到真实程序里

- 包、模块与项目布局
  - 包与项目清单
  - 用模块控制作用域与可见性
  - 在模块树中引用条目的路径
  - 使用 `import` 与限定名
  - 把模块拆到不同文件中

- 数组、缓冲区与视图
  - 定长数组
  - 把文本当作 `char` 与 C 兼容缓冲区处理
  - 用 `std::span` 借用视图

- 错误处理
  - 不可恢复错误与编译器插入的检查
  - 今天可用的可恢复错误写法
  - 为 `std::expected` 做准备

- 泛型代码、concept 与生命周期
  - 泛型数据类型
  - 用 concept 描述共享要求
  - 用生命周期验证引用

- 编写自动化测试
  - 编译并运行式测试
  - 控制测试命令
  - 测试组织方式

- 一个 I/O 项目：构建命令行程序
  - 接收命令行参数
  - 读取文件
  - 重构成模块
  - 用测试增加功能
  - 处理环境变量
  - 把诊断信息写到标准错误

- 闭包与显式迭代
  - 闭包
  - 用循环和视图处理一串数据
  - 继续改进命令行项目
  - 显式循环的性能

- 进一步理解 project build 与可复用包
  - 编译器模式与项目构建模式
  - 构建可复用的模块制品
  - workspace
  - 安装与运行二进制程序
  - 扩展工具链

- 智能指针与拥有型句柄
  - 使用 `std::unique_ptr<T>`
  - 像用引用一样使用拥有型指针
  - 用析构函数执行清理代码
  - `std::shared_ptr<T>`
  - 用 `mutable` 做内部可变性
  - 避免引用环与所有权混乱

- 无畏并发
  - 用线程同时运行代码
  - 安全地跨线程移动数据
  - 共享状态并发
  - 线程 trait：`thread_movable` 与 `thread_shareable`

- 互操作与固定布局数据
  - 固定布局的 `struct` 值
  - C ABI 边界
  - packed 布局与 `union` 逃生舱

- 高级特性
  - 高级 concept 与约束
  - 高级类型与函数指针
  - 高级函数与闭包
  - 没有宏时如何做元编程

- 最终项目：构建一个多线程 Web 服务器
  - 先做单线程 Web 服务器
  - 从单线程走向多线程
  - 优雅关闭与清理

- 附录
  - A - Attribute 与保留写法
  - B - 运算符与符号
  - C - 标准库基础构件
  - D - 常用开发工具
  - E - SCPP26 与 C++26 基线
  - F - 本书翻译版本
