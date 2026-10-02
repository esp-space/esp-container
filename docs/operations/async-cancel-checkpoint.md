# 异步取消检查点

2026-10-02，本仓公开组件接入 WAMR `74fd95ccbdc417c3816e04f3308eea8a5473ed34` 的 Classic owner 取消谓词。本检查点只证明真实宿主解释器与组件合同，不代替 Base 签名产品、固定 SDK 编译、两板管理面响应或五能力动态容量验收。

## 实际问题与实现

旧 WAMR 的 `wasm_runtime_terminate` 在 guest 线程管理关闭时只设置模块异常，纯 Wasm 长循环仍持续至墙钟期限。旧控制探针以 500 ms 期限返回，取消请求后仍运行约 358 ms，异常是墙钟超期。这不能证明异步取消已经生效。

新 WAMR 接口由执行 owner 在 guest 调用前设置、返回后清除。其他宿主线程仅修改调用方拥有的 C11 原子状态，不写模块异常、执行环境或 64 位期限字段。谓词有界、不阻塞、不重入，context 保持有效至 `product_close`。Classic 在首条和至多相隔 256 条 opcode，以及原生入口边界协作检查；已有 guest/native 异常保持。guest pthread、AOT/JIT、Fast Interpreter 继续关闭。

公开 `econtainer_runtime_limits_t` 新增 `cancel_requested` 与 `cancel_context`；包槽仍按原平台配额与签名清单取交集，完整复制这两个 owner 字段。签名包本身不能配置平台谓词或获得原生指针。返回 `ECONTAINER_RUNTIME_ENTRY_CANCELLED` 后拒绝继续业务入口，取消实例定时器，回滚本入口的待取日志，保持调用前日志。调用栈完全退出后允许实际执行 `product_stop`，停止入口忽略尚未清除的取消请求，但保留自己的正数指令预算和墙钟期限。停止失败不能作为可启动新实例的证明。

## 验证

- 同一公开 WAMR 提交的 Classic 期限与取消 CTest：goto／switch 各 2/2，均启用 ASan/UBSan；新增原生入口取消、已有 native trap 保留和清除谓词后恢复指令预算的回归。
- 本仓完整 CTest：goto／switch 各 9/9，均启用 ASan/UBSan。真正的 native 请求线程分别中断 init、事件与含日志／定时器的事件；收到取消结果，未交付成功 guest result，后续业务入口拒绝。停止死循环仍超期，停止返回失败仍失败。
- 各分派另运行 100 次真实取消／关闭／重开循环，未用墙钟超期代替取消。
- macOS 普通构建启用已校准的 malloc／虚拟地址探针；取消循环第 10/50/100 次关闭后，malloc 为 11,104／11,104／11,104 B，虚拟地址用量为 445,751,787,520／445,751,787,520／445,751,787,520 B，区域数为 62／62／62。虚拟地址指标不是 RSS，也不是 MCU 可用 RAM。既有普通、原生导入、定时器和故障重开各 100 次回归保持通过。

普通宿主构建入口仍使用 README 的 WAMR／wasi-sdk 参数；switch 对照额外设置 `-DCMAKE_C_FLAGS=-DWASM_ENABLE_LABELS_AS_VALUES=0`，ASan/UBSan 在 compiler flags 中显式开启。wasi-sdk 33 官方 macOS arm64 归档 SHA-256 为 `85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78`。两目标锁由与固定 SDK 相同的官方 Component Manager 3.1.2 解析公开精确 SHA 生成，没有在构建中注入补丁或修改下载后的源码。

同步阻塞 native import 与 OS 调度不可硬抢占；测试的 250 ms 请求后观察界限不构成 MCU 硬实时承诺。Base 产品 owner、真实签名包、实体板和正式发布继续独立记录，P6-07 保持进行中。
