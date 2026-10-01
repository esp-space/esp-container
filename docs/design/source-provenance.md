# 来源与许可

本仓第一方 `esp_container` 检查器、guest SDK、主机打包工具和测试采用 Apache-2.0。没有复制旧 ESP Service、ESP Tool 或第三方运行时代码。

| 来源 | 精确版本 | 用途 | 本仓保留范围 | 许可 |
| --- | --- | --- | --- | --- |
| [WAMR 公开维护 fork](https://github.com/darren-you/wasm-micro-runtime) | `c10736fffdf26d7c2ae234e05aa712df112eb6bf`，直接父提交 `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`，官方祖先为[官方 WAMR-2.4.4](https://github.com/wasm-micro-runtime/wasm-micro-runtime/tree/8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629) `8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629` | ESP-IDF 组件依赖、Classic Interpreter、指令计量、解释器栈/单元对齐修复与协作式墙钟期限 | 不复制源码；由 IDF Component Manager 从公开 fork 按完整 SHA 获取 | Apache-2.0 WITH LLVM-exception |
| [ESP-IDF 公开维护 fork](https://github.com/esp-space/esp-idf) | `578cf89c343e388db43ba1f4ddcd602fedcb763c`；直接父提交 `855937cf9dcee13ee9c423fb0319238cdc8d53fd`，官方祖先提交 `fff9895c82d744c7237be8847347bdd1b07c6643`；修复 OTA 擦除失败和 HTTP 客户端初始化失败时的传输句柄泄漏 | C3 工具链与固件 SDK | 不复制源码；独立 SDK checkout | Apache-2.0 等，依上游各文件 |
| [esp-lwip](https://github.com/esp-space/esp-lwip) | `2758df4cd3666b3b2a5b53830148379326425c0d` | 目标组合的 SDK lwIP 修正 | 不复制源码；随锁定 IDF checkout | 见该仓许可 |
| [wasi-sdk 33](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-33) | macOS arm64 资产 SHA-256 `85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78`；`VERSION=33.0+m`，LLVM `4434dabb6991` | counter guest 的 freestanding Wasm 编译 | 工具链放在仓外；仓内只保留编译命令、版本核对和输出检查 | 见上游各组件许可 |

WAMR 公开 fork 的 `a34d721` 修复三处 ESP-IDF 源码/构建文件：未开启 WASI 时不编译文件适配、补齐 POSIX stat 类型声明，以及目标 SDK 无可执行堆能力时拒绝执行映射。`26c235e` 在同一来源链修复 Classic 分支帧和 64 位 cell 未对齐访问，并同步共用 cell 布局的编译路径。`c10736f` 增加按执行环境设置的 Classic 协作式墙钟期限和独立回归；执行线程每至多 256 条 opcode 检查一次，原生导入不能在阻塞中被中断。修改直接进入 WAMR 源码，没有在本仓或 SDK 使用构建时补丁，也没有关闭 C3 内存保护。上游 WAMR 的 C3 示例仅作为组件装配和 API 行为依据。本仓 `examples/c3-runtime` 的两份最小 Wasm v1 字节由对应函数体手工构造，用于预算与死循环原型，不是上游示例字节的复制。WAMR 的默认 IDF 配置含 Fast Interpreter、AOT 和 WASI；本仓样例显式关闭并构建检查。WAMR v2.4.4 的实例化路径会调用特定导出入口，所以本仓在加载前拒绝 start 段及 `__post_instantiate`、`__wasm_call_ctors`、`_initialize` 导出；这项检查的安全性仍须真实运行与异常输入验证。
