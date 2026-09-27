# 单实例运行切片检查点：2026-09-23

当前 C3 分支已硬切到 ABI 2 页内事件区并取消附加 host heap；新合同与验证见[单页 profile 的 ABI 2 检查点](c3-low-memory-profile.md#abi-2-页内事件区与标准页边界)。下面各节保留旧四导出/附加 heap 的历史测试输入，不再作为当前 SDK 接线示例。

## 初始组件内合同

`components/esp_container/src/runtime_internal.h` 是组件私有边界。它接收内存中的 Wasm 与显式资源限制，仅用于当前可复现的软件检查点；公开头 `include/esp_container.h` 没有裸 Wasm 安装入口，Base 不能直接装配这套函数。

2026-09-24 的 C3 候选另加组件私有 `slot_runtime_internal.h`：通过现有槽锁读取真实确认绑定或本 boot 已持久的 `TRIAL_STARTED`，对映射的同一完整包重新验签、授权和静态扫描，再调用私有 `open` 并立即解除映射。它不执行 `init`，不新增持久相位或租约；完整合同和新增真实签名集成证据见[三槽存储检查点](three-slot-storage-checkpoint.md)。以下原始运行切片验证保留其当时边界；当前按节复制与输入生命周期见[单页 profile](c3-low-memory-profile.md)。

`open` 要求输出指针预先为 `NULL`，拒绝第二个实例，并复制一份供 WAMR 持有的可写模块字节。加载前先做现有无 import、无 start、无隐式构造入口扫描，再从原始 Wasm memory section 核对单块非共享、有限最大页数；WAMR 加载后核对四个且仅四个导出：`memory`、`econtainer_init() -> i32`、`econtainer_on_event(i32, i32) -> i32`、`econtainer_stop() -> i32`。WAMR 会规范化加载后的内存页数，所以不能拿其导出类型中的页数替代原始 section 上限检查。

成功路径是 `open → init → on_event* → stop → close`。`init`、`on_event`、`stop` 每次调用都设置各自的正数指令预算；额度耗尽与其他引擎异常分别返回。`init` 和 `stop` 要求 guest 返回 0；`on_event` 的 guest 返回值只在宿主状态为成功时写给调用者，允许负数。事件先通过 `wasm_runtime_module_malloc` 分配 guest 地址、复制 payload，再在调用后释放；分配失败返回独立的内存不足状态，不改变运行态。`stop` 成功后可重复调用；一旦 guest trap 或 init/stop 返回失败，实例进入不可继续执行的失败态，`close` 仍释放 exec env、实例、模块、可写字节和 WAMR 全局所有权。`close` 也可用作未停止实例的立即释放；当前 guest 无宿主能力和异步回调。

ESP-IDF 调用方必须在**同一个由 `pthread_create` 创建的宿主线程**串行执行这些方法。当前锁定 WAMR 的 `espidf` 平台实现将 `os_self_thread` 映射到 `pthread_self`；在普通 FreeRTOS `xTaskCreate` 任务中调用会触发断言。全局原子占用只拒绝并行打开第二实例，不提供同一实例方法的并发保护。

`max_wasm_bytes`、guest 声明的 `max_memory_pages`、stack、host-managed heap、最大单事件字节数和三个指令预算目前都是调用方输入，尚无产品冻结值。页数上限不是 C3 总 RAM 峰值；模块副本、WAMR 内部元数据和 host-managed heap 还会占用内存。

## 已完成验证

使用固定 wasi-sdk 33 生成真实 counter 与五个临时 guest，使用精确锁定的 WAMR Classic 源码运行 `ctest`，三项全部通过。`runtime_instance` 逐项覆盖 counter 事件计数、guest payload 读取、空事件负返回值、过大事件拒绝、guest 堆分配不足后恢复、单实例占用、错误 ABI、超过原始内存页上限、三个入口分别死循环触发精确指令额度异常、失败释放、重复 stop/close 与八轮关闭后重开。输出 Wasm 位于 CMake 构建目录，不进入 Git。

固定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、WAMR `a34d721b630213f59fde0b40cebbb980903660e8` 的 C3 样例完整构建通过，新 `runtime.c` 被编译进组件 archive。原样样例只调用先前底层探针，其最终 ELF 不保留这个新 API。另在仓外复制的临时工程强制引用私有 `open`，完整链接通过，最终 ELF 的符号表保留 `open/init/on_event/stop/close` 和 WAMR `module_malloc`；这项链接检查本身不证明实际执行。临时工程和生成镜像不进入仓库。

随后在同一仓外工程 `/tmp/esp-container-single-instance-link-probe`（源于本仓 `4f70873f4460bfb1b7a3c6c2efe1067dc6040779`）的 `main` 临时嵌入本次 wasi-sdk 生成的 counter 与事件死循环 Wasm，并让同一个 `pthread_create` 宿主线程依次调用私有 API。固定 SDK 构建的 C3 仿真镜像为 236400 字节，SHA-256 `9cd51cf900ad41fd383c27720ddae5ba67052982a002b89dc3a376cfe266bcf3`。官方 Espressif `qemu-riscv32` 9.2.2 (`esp_develop_9.2.2_20260417`) 串口实际输出 `private-runtime counter=0/0/0/0 result=3 loop=0/0/8`：四个 0 依次是 counter 的 open/init/on_event/stop 成功，`result=3` 是三个事件字节后的 guest 计数；loop 的两个 0 是 open/init 成功，8 是 `ECONTAINER_RUNTIME_INSTRUCTION_LIMIT`。counter close 后同一线程重开 loop 并关闭，两次释放路径均返回。仿真还保留原样底层探针的正常返回和精确额度异常。日志位于仓外 `/tmp/esp-container-single-instance-qemu-official.log`，临时源码、Wasm 和镜像均不提交。

## 未闭合边界

当前没有宿主能力授权、guest 句柄、异步回调、签名 `product.pkg` 设备验包、Flash 三槽、Base 装配与实例持久状态；指令计量也不能单独限制未来宿主函数内部的耗时。官方 QEMU 仿真不能代替真实 C3 设备及五组件组合的动态 RAM 峰值、实板运行和掉电恢复证据。P6-04 与 P6-07 仍未验收。

后续在[宿主导入检查点](host-api-checkpoint.md)增加了逐项授权的单调时间与有界日志私有切片；本节保留 `4f70873` 的原始检查事实，完整 P6-04/P6-07 仍未验收。

2026-09-24 又增加私有 `max_entry_duration_ms`：原生导入前和 WAMR 返回后检查单调时钟，超期实例进入失败态，调用方不能收到成功 guest 结果。详见[宿主导入检查点](host-api-checkpoint.md)；同步 SDK 调用不能由此抢占，P6-07 的完整宿主调用硬期限仍未验收。

2026-09-27 以精确 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf` 增加 Classic 执行环境的协作式期限检查，私有运行期对 `init/on_event/stop` 分别设置并清除绝对微秒期限。真实纯 Wasm 循环在指令额度充足时先返回 `ENTRY_EXPIRED`，失败实例仍须由 owner `close`；具体异常、检查间隔与原生导入限制见[宿主导入检查点](host-api-checkpoint.md)。这没有改变事件、日志、计时器或签名包的所有权，不能把解释器协作检查写成同步原生调用的硬中断，也不能替代两板实测。

## P6-09 host 重复释放切片（2026-09-24）

当前同步私有运行期的资源归属经代码核对：`stop` 成功后进入 `STOPPED`，指令额度异常或 guest 非零返回后进入 `FAILED`；两个状态都可由 `close` 统一销毁 WAMR exec env、实例、模块，注销原生导入，释放待取日志与可写模块副本，销毁 WAMR 全局状态并归还单实例占用。事件调用结束时，无论 guest 是否 trap，宿主均释放本次 guest 堆分配。当前只有同步时钟/日志导入，没有可迟到的异步回调或原生句柄；因此此处未发现需修复的释放分支错误。

固定 wasi-sdk 33 与锁定 WAMR `a34d721b630213f59fde0b40cebbb980903660e8` 的 `runtime_instance` 现执行三组真实 Wasm 回归：100 次 counter 的 `open→init→event→stop→close`；100 次含授权时钟/日志导入、日志取走的完整生命周期；100 次轮换 init/event/stop 指令额度异常和 stop 非零返回，每次失败关闭后立即重开 counter 并完成事件、停止与关闭。普通 host CTest 和 ASan/UBSan 均通过。macOS 普通构建先在测试进程内分别注入 64 KiB `malloc` 与 `mmap`，要求对应探针读到至少 64 KiB 增量，再在每组第 10、50、100 次关闭后读取默认 malloc zone 的在用字节、`TASK_VM_INFO` 虚拟地址用量与 VM 区域数：本次 counter 堆 `23696/23696/23696`，失败后重开 `26064/26064/26064`，原生导入 `24016/24016/24016` 字节；三组虚拟地址用量在各自三个采样点持平。VM 区域数在第 50→100 次不增加；个别先前运行曾于前 50 次由 61 增至 63，但虚拟地址用量仍持平。该 host 的 `leaks --atExit` 对故意泄漏也报告 0，故不作为证据。

这些读数仅证明此宿主、当前同步导入和当前 guest 集合在重复关闭后没有持续增加已测资源。P6-09 仍未验收：未来消息/设备/存储等异步宿主回调须有明确的取消、完成与迟到拒绝合同；真实 C3 板还须在 Base 装配、MQTT/FRP/OTA 同时运行及真实包槽条件下测量停止期限、heap/连续块和原生资源，并验证失败停止时实例已停止回收后才允许下一实例。
