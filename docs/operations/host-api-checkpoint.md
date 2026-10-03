# 宿主导入检查点

## P6-07 签名单次宿主期限接线（2026-10-04）

公开 `econtainer_runtime_limits_t` 现在要求正数 `max_host_call_timeout_ms`，所有调用方必须显式装配。`econtainer_product_open` 在同一槽锁内重新验签后，将该平台值与本次真实清单的 `host_call_timeout_ms` 取最小值；独立的 `max_entry_duration_ms` 保持原值。签名不能放宽平台授权，也不能把单次宿主期限换成整个 guest 入口额度。

四个导入在确认执行 owner 后保存本次单调起点；操作函数的全部返回路径，包括非法参数、日志繁忙和定时器拒绝，均经过返回时期限检查。下一次调用重新计时，合法短调用的耗时不累计到单次额度。已有 WAMR 异常保留；正常返回时维持入口超期、owner 取消、单次宿主超期的优先级。`HOST_CALL_EXPIRED` 使实例失败、不交付 guest 结果，沿原入口超期路径保留旧日志、丢弃本入口新日志并停用全部实例 timer。新增宿主开始/结束时钟失败、回退及期限加法溢出使用相同清退，结果仍为 `ENGINE_FAILURE`；body 原有引擎异常继续遵守原合同，没有扩大到所有 generic trap。调用方仍须显式关闭实例。

此实现检查实际原生调用的开始与返回时刻，不能在同步原生操作阻塞中抢占，也没有新增 worker、线程、计时任务或运行期时钟回调。调度影响仍可能使返回晚于配置额度；后置拒绝成功结果不能证明硬返回上界或撤回未来原生导入可能产生的外部副作用。当前四个导入只读时钟或操作实例内日志、timer，未增加外部操作。

`host_call_timeout` 使用固定 wasi-sdk 33 的真实四导入 Wasm、临时 RSA-3072 测试键、公有签名包工具和实际公开槽装载链。仅测试库编译替换 `runtime.c` 的 POSIX 时钟符号，WAMR 自身仍使用真实平台时钟。回归包含 signed/platform 双向 min、>= 期限边界、非累计短调用、四导入成功与拒绝返回、init/stop、原异常与 entry/cancel 优先、旧日志保留、新日志清退、失败后关闭重开和时钟失败/回退/溢出。FAILED 状态的公开 timer 接口返回 `INVALID_STATE`，这一返回只证明不能投递；inactive 位清理由实际源码的清退循环保证，关闭重开另检查无 timer。

红测只扩展公有字段和真实 guest/签名夹具、保持旧运行行为，已命中“签名 100 ms、平台 500 ms 的合法日志调用没有返回 `HOST_CALL_EXPIRED`”断言。macOS 断言子进程没有及时完成，CTest 记录为 30 s Timeout 并保留该断言；Timeout 本身不作为期限证据。首默认 Python 缺少 `cryptography` 的构建失败和测试夹具接线修正均保留，最终验证使用已有受测 Python 环境。最终统一源码的普通 goto、ASan/UBSan goto 与 switch 三配置全量 CTest 各 **10/10**，构建及测试命令均退出 0；sanitizer 设置 `ASAN_OPTIONS=halt_on_error=1` 与 `UBSAN_OPTIONS=halt_on_error=1`。最初相关三项的 3/3 只覆盖 `runtime_instance`、`slot_runtime`、`host_call_timeout`，不冒充全量。未重新声明历史 Python/Go/浏览器包格式测试，也未以本次软件测试宣称新 SDK、实板期限、容量或完整 P6-07 验收。

## 当前私有 ABI

固定 wasi-sdk 33 编译的 guest 可从 `econtainer` 模块导入 `monotonic_ms() -> i64`、`log(i32 offset, i32 size_bytes) -> i32`、`timer_start(i32 delay_ms, i32 period_ms) -> i64` 和 `timer_cancel(i64 handle) -> i32`。guest SDK 声明对应的 `econtainer_` 函数。C 扫描器与主机包工具均拒绝其他模块、函数、种类、函数签名和重复导入。`counter.c` 仍为无导入样例；`tests/host_api_guest.c` 与 `tests/timer_guest.c` 分别运行时钟/日志和定时器导入。

`components/esp_container/src/runtime_internal.h` 仍是组件私有接口。调用方必须显式给 `allowed_capabilities` 授予对应的时钟、日志和/或定时器能力；未授权的必需导入在 WAMR 加载前拒绝。包工具还要求实际导入对应的 `monotonic-time`、`log`、`timer` 位于已签名 manifest 的 `required_capabilities`，但这只核对包声明，不授予设备权限。公开产品入口只从实际选中槽重新验签装载；Base 不能直接消费裸 Wasm 私有入口。

日志导入在当前实例中只复制一条待取记录，长度上限由 `max_log_bytes` 设置且不超过 256 字节；零长度和超限返回 `-1`，上一条尚未取走返回 `-2`，有效日志返回 `0`。WAMR 验证偏移和长度属于**当前实例**的 guest 地址空间；越界地址触发引擎异常并使该实例失败。宿主在 guest 调用返回后通过 `econtainer_runtime_take_log` 复制并取走日志；目标缓冲太小时记录不丢失。`close` 释放缓冲，新实例没有旧日志或可复用句柄。原生导入使用 WAMR 执行环境与实例身份核对，不暴露宿主地址。

四个导入只执行单调时钟读取、最多 256 字节的内存复制或固定八槽内的有限操作，不发起网络、Flash、等待或用户回调；guest 调用链中没有同步反向调用 guest 的入口。`init/on_event/stop/take_log/poll_timer/close` 使用单实例调用占用位拒绝同一时段的第二次进入。ESP-IDF 上仍要求同一 `pthread_create` 宿主线程串行调用；占用位不是跨线程实例生命周期管理。日志输出、敏感值过滤和速率限制尚未连接，待取字节不能直接当作安全的产品日志发布。纯 Wasm 的协作式期限检查与同步原生调用的可取消性边界见下文；完整宿主墙钟期限机制仍未验收。

## P6-07 入口期限软件切片（2026-09-24）

组件私有运行限制现要求调用方提供正数 `max_entry_duration_ms`。每次 `init/on_event/stop` 在调用 WAMR 前读取单调时钟；原生导入先检查本次入口是否过期，过期时不再执行该次导入；WAMR 返回后再次检查，超期的 guest 返回值不交给调用方。超期使实例进入 `FAILED`，取消其所有实例定时器，并丢弃本次入口产生、尚未取走的日志；调用方仍须 `close`。保留此前成功入口留下的待取日志。当前日志只有实例内待取槽，定时器只有实例内槽且无后台回调；入口占用期间外部不能取日志或轮询定时器。已交给宿主的先前日志和事件结果、未来新增导入可能已完成的外部副作用均不能事后撤回。指令预算继续独立生效。

截至本段日期，该实现只做**成功结果的期限裁决**，没有解释器执行中的期限检查。WAMR 与 ESP-IDF 同步原生调用在执行期间不能由这个检查中断；没有原生导入的 guest 只会在返回后受到时间裁决，实际返回耗时仍取决于指令预算与调度。当前四个导入均为有限、无等待操作，因此此切片改善了入口的超期成功误报和后续导入副作用边界；未来若加入网络、Flash、等待或其他可能阻塞的 SDK 调用，必须另行设计可取消的异步合同。截至本段 2026-09-24 检查点，签名 manifest 的 `host_call_timeout_ms` 仅用于包槽独立限额准入；2026-10-04 的软件接线见文首，旧检查点没有证明该运行期合同。

固定 wasi-sdk 33 与锁定 WAMR 的真 Wasm 测试使用同一 `deadline.wasm` 覆盖两种路径：循环导入时钟跨过期限，下一次导入被拒绝；仅在开头导入日志、随后纯 guest 计算跨过期限，WAMR 返回后拒绝结果。两者均检查 `ENTRY_EXPIRED`、调用方结果未改、实例失败、本次日志不可取及失败实例可关闭。另先成功建立定时器和待取日志，再让后续入口超期，检查失败态拒绝计时轮询且先前日志仍可取。同一 Wasm 对照旧运行期时，超期事件实际返回 `status=0`（`OK`）、`guest_result=0`，新运行期消除了这一误报。主机 Python unittest 18/18、锁定 WAMR CTest 8/8 通过；新版固定 SDK 的 C3 原样样例也完整构建，`runtime.c` 编进组件 archive，app 为 `0x39360` 字节、SHA-256 为 `94672e1d4529a32758b4fbcdfab5ef6fbea1634a243d59dc8a6109aaf3298131`。该样例没有调用私有入口，以上主机测试与构建均不能证明 C3 实板的调用返回时限或未来 SDK 阻塞调用的取消。

## P6-07 真实中断能力复核（2026-09-27）

本轮只核对 `esp-container@3b5f16f01aaf4695b514b1f5f81b21e4abbd85cd`、锁定的 [WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`](https://github.com/darren-you/wasm-micro-runtime/tree/26c235e53e29acd8b43abe7f3b524577bd4d1ae5) 与当前 C3 profile，没有更改运行源码或设备。固定 wasi-sdk 33 生成 `tests/runtime_guest.c` 的 `init-loop.wasm`、`event-loop.wasm`、`stop-loop.wasm` 和正常 `counter.wasm`。仓外 C 探针沿用 `tests/runtime_instance_test.c` 的运行限制，只把三个入口的指令额度分别设为 `10000000`、`max_entry_duration_ms` 设为 `1`；逐个打开死循环模块，event/stop 先正常 init，再用 `clock_gettime(CLOCK_MONOTONIC)` 测入口调用耗时。每次失败后执行 `close`，立即打开正常 counter 并完成 `init → stop → close`。期望的墙钟硬期限断言是入口在 1 ms 内结束；旧实现的实测结果为：

| 真 Wasm 入口 | 返回码 | 入口耗时 | 失败后关闭与重开 |
| --- | --- | ---: | --- |
| init 无限循环 | `INSTRUCTION_LIMIT`（8） | 34 ms | 成功 |
| event 无限循环 | `INSTRUCTION_LIMIT`（8） | 23 ms | 成功 |
| stop 无限循环 | `INSTRUCTION_LIMIT`（8） | 20 ms | 成功 |

这些耗时是本机单次读数，不是设备性能指标；可复现的失败事实是三次调用均越过 1 ms 才返回，且先命中指令额度。正数指令预算确实让这三个纯 Wasm 无限循环最终退出，`close` 也能在退出后回收并重开；额度消耗需要的墙钟时间受主机执行与调度影响，不能从指令数推出硬期限。并行 `close` 会被 `call_active` 拒绝，实际销毁仍须由同一 owner 等入口返回后执行，不得强行释放仍被 WAMR 引用的实例。

固定 WAMR 的 [运行构建规则](https://github.com/darren-you/wasm-micro-runtime/blob/26c235e53e29acd8b43abe7f3b524577bd4d1ae5/build-scripts/runtime_lib.cmake)会在启用 lib-pthread、WASI threads 或调试解释器时引入线程管理；当前组件拒绝 guest pthread/调试解释器，C3 `sdkconfig.defaults` 关闭 pthread 和共享内存，主机测试配置也未启用线程管理。`core/config.h` 因此使用 `WASM_ENABLE_THREAD_MGR=0`。在这个精确 profile 中，[`wasm_runtime_terminate`](https://github.com/darren-you/wasm-micro-runtime/blob/26c235e53e29acd8b43abe7f3b524577bd4d1ae5/core/iwasm/common/wasm_runtime_common.c)只写实例异常；无线程管理时异常锁为空操作，[Classic 解释器](https://github.com/darren-you/wasm-micro-runtime/blob/26c235e53e29acd8b43abe7f3b524577bd4d1ae5/core/iwasm/interpreter/wasm_interp_classic.c)也不检查终止标记。不能从该 API 的通用声明推断当前构建可以跨线程安全、及时地终止纯 guest 循环。

当前四个原生导入只读单调时间、复制最多 256 字节日志，或扫描最多八个实例定时器槽；源码没有等待、网络、Flash 或调用方回调，也没有已证实的同步阻塞导入。它们是有限操作，但目前没有在固定 IDF/C3 调度下证明各自的墙钟上界。WAMR 的 [`begin_blocking_op/end_blocking_op`](https://github.com/darren-you/wasm-micro-runtime/blob/26c235e53e29acd8b43abe7f3b524577bd4d1ae5/core/iwasm/common/wasm_blocking_op.c)在无线程管理或没有平台阻塞唤醒支持时为空操作；固定 WAMR 的 ESP-IDF 平台未声明 `OS_ENABLE_WAKEUP_BLOCKING_OP`。因此未来若增加可能阻塞的同步宿主导入，不能靠跨线程 `terminate` 或当前入口的返回后检查保证取消及资源回收。

本轮独立主机构建使用上述精确 WAMR 和 wasi-sdk 33，CTest **9/9**、Python unittest **20/20** 通过；仓外探针在旧实现上两次复现三入口超期，故未把现有事后期限裁决写成硬超时，也没有添加无消费者的异步 worker 或线程强杀。P6-07 的真实入口墙钟有界结束仍未完成。下一步须先在 WAMR 源码边界证明适用于此 Classic profile 的安全中断点及 C3 调度/资源成本，再以精确 WAMR 提交更新组件锁并验证 init/event/stop 和失败回收；任何实际新增的可能阻塞原生操作还需对应 SDK 支持的可取消执行合同，不得预先假设平台可以唤醒它。

## P6-07 Classic 协作式期限接线（2026-09-27）

公开 WAMR fork [`c10736f`](https://github.com/darren-you/wasm-micro-runtime/blob/c10736fffdf26d7c2ae234e05aa712df112eb6bf/tests/unit/classic-deadline/README.md) 将绝对微秒期限绑定到所属执行环境。仅在 Classic、指令计量开启且 Fast/AOT/JIT、线程管理、调试解释器、共享内存关闭时允许启用。解释器在首次 guest opcode 前及之后每至多 256 条 opcode 检查期限；返回 native 前再检查，直接重导出的原生入口在调用前后检查。已有异常保留原异常；期限与指令额度在同一检查点耗尽时优先报告期限。Container 在每次 `init/on_event/stop` 前用与该平台 WAMR 相同的单调时钟设置期限，返回后清零；精确期限异常映射为 `ENTRY_EXPIRED`，沿用本次待取日志丢弃、计时器撤销、实例失败及显式 `close` 合同。

WAMR 的先红后绿真 Wasm 回归中，旧版三入口的 1 ms 期限均延至约 16 ms 后才以指令额度结束；新提交的 goto/switch 两种分派均通过循环、`br_if`、`br_table`、递归、长直线、原生重导出、清除/重开及异常优先级测试。Container 的真 Wasm `runtime_instance` 另以高指令额度和 20 ms 期限覆盖三个纯 guest 死循环，每次须得到 `ENTRY_EXPIRED`、失败态且关闭后可重开正常 counter。测试时长和 opcode 检查间隔不是硬实时承诺；单条 opcode、OS 调度及正在运行的同步原生导入均可能使调用在期限之后返回。固定 ESP-IDF 的 WAMR 取时直接使用 `esp_timer_get_time()`；POSIX 后端若时钟调用失败，旧 API 返回 0，期限会退化到指令额度。这一 host 平台剩余边界已记录在 WAMR 检查点，不能解释为设备端已验证。

组件仍只有无等待的四个原生导入。若以后增加可能阻塞的网络、Flash、等待或外部回调，需该调用自己的可取消执行与资源回收合同；本次解释器检查不能抢占它们。签名包的 `host_call_timeout_ms` 尚未绑定到设备公开安装/运行入口，实际 C3/ESP32 产品同存时延和失败回收还须实板验证，P6-07 继续进行中。

固定 IDF `578cf89c`、lwIP `2758df4` 下，两目标独立样例执行官方 `update-dependencies` 后的锁都指向完整 WAMR `c10736f`，组件 hash 均为 `713ac0a363a97ad951440e9f5a495d5cac426ee5501667a61d8aed6291c3facc`；编译命令核实 `wasm_interp_classic.c` 与 Container `runtime.c` 都收到期限宏。C3 样例 app 为 `0x35950`、SHA-256 `85298786bc17c99fd3b6adbe4d96e9cef163a505ba7e5243d69c1b982bc5e68d`；ESP32 为 `0x33b40`、SHA-256 `b9de836671fb1968456925002a33324c9bb31a175ecc9f9e98de59ad5e87df00`，两者均完整编译、链接并通过官方分区尺寸检查。锁定源码和 wasi-sdk 33 的 Container 主机 CTest 普通、ASan/UBSan 各 **9/9** 通过；真实 guest 覆盖三个纯计算入口的期限、失败回收和正常重开。样例自身没有设置 deadline 或调用公开产品运行入口，不把构建当成板上墙钟证据；测试没有刷板。

## 2026-09-24 定时器软件切片

平台在运行限制中独立授予 `ECONTAINER_CAP_TIMER`，并设置 `max_timers` 为 1–8；未授权模块在 WAMR 加载前被拒绝。guest 可创建 1–86400000 毫秒后的一次或周期事件，周期为 0 时只触发一次。返回的 64 位句柄来自进程内跨实例单调计数器，取消或关闭后也不复用；用尽 `UINT64_MAX` 后保守拒绝再创建，0 始终无效。取消逐槽核对本实例仍活跃的完整句柄，旧实例和已到期的一次性句柄返回 `-1`。停止先撤销全部定时器，再调用受预算约束的 guest stop，stop 中不能创建新定时器；关闭实例不留下原生计时器回调或后台任务。

定时器不运行自己的线程。唯一 owner 在 guest 入口之外通过 `econtainer_runtime_next_timer_deadline` 获取下一次唤醒点，并在到期时调用 `econtainer_runtime_poll_timer`；每次最多投递一条到普通 `econtainer_on_event`，使用同一条事件指令预算。宿主导入只登记或取消，不同步重入 guest。事件为 16 字节固定编码：`ECT`、版本 1、LE64 句柄、LE32 已合并跳过的周期数；普通事件入口拒绝这一保留格式，避免伪造。周期超时只投递一条，下一期限沿原节奏推进；guest 内存分配失败时保留原到期状态，供 owner 再试。此机制不承诺硬实时精度，若 owner 不轮询就不会自动投递。

锁定 WAMR 真 guest 回归覆盖一次/周期事件、周期超期合并、配额满、非法期限、取消后新建不复用、伪造事件拒绝、stop 撤销和跨实例旧句柄拒绝。句柄分配函数的边界测试直接跨过旧 24 位序号上限，并验证 `UINT64_MAX` 最后一张有效句柄及其后的永久拒绝，无需循环千万次。平台尚未提供公开安装入口或 Base 装配，因此此切片不能被解释成设备上的产品定时任务已可发布。

隔离 checkout 使用 wasi-sdk 33 的 Python unittest 18/18、锁定 WAMR 的普通与 ASan/UBSan CTest 各 8/8 通过。定时器 guest 连续 100 次创建、挂起计时、stop、close；macOS 校准后的第 10/50/100 次 malloc 在三个采样点均为 24272 字节，虚拟地址均为 500517584896 字节，区域数均为 63。固定 ESP-IDF `855937c` 与 lwIP `2758df4` 的 C3 原样样例构建通过，app 为 `0x39260` 字节，SHA-256 为 `9f30d72c7b77d1b17f95edf0db6073baa2e68967e7efdbf0b9c4f150938984f6`；该镜像没有执行定时器 guest，也没有刷板。

随后在独立 checkout 临时扩展 C3 样例的同一个 `pthread_create` owner，嵌入由固定 wasi-sdk 33 编译的 `tests/timer_guest.c`（Wasm 934 字节，SHA-256 `f1b72ab3efd0ed88d6ceda53caec953fffb7e257bfdc7cb76e3dcff8aa1a6a99`），使用公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd`、esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 和锁定 WAMR 构建。临时镜像大小 `0x3b510` 字节，SHA-256 `7080848b456efc0dfe771092a274ba99b1bd0dcba294463b805f9c7717b5b0c7`。官方 Espressif QEMU 9.2.2（`esp_develop_9.2.2_20260417`）的 C3 串口显示 `open/init/cancel-now/replacement/schedule/early poll/one-shot/periodic/coalesced periodic/cancel periodic/stop/close` 全部返回预期状态，周期合并 `skipped=1 result=21`，最终 `timer path=1`。打开前、打开后、关闭后的 8-bit 空闲堆分别为 324520、176164、324520 字节，关闭前后最大连续块均为 188416 字节。该修改只用于仓外临时仿真，正式样例保持原入口；QEMU 结果不等于实板时序、签名包安装、Flash 三槽或 Base 同时运行验收。

## 已验证的软件证据

在独立 checkout 中，固定 wasi-sdk 33 的 Python unittest 18/18 通过；锁定 WAMR `a34d721b630213f59fde0b40cebbb980903660e8` 的普通及 ASan/UBSan CTest 均为 3/3 通过。真实 guest 覆盖逐项拒绝未授权导入、日志复制后原事件缓冲改写、短目标缓冲保留、队列满、长度超限、非法地址异常、单调时间以及关闭重开不继承日志；包工具用测试键验证导入需求必须进入签名 manifest。

此前使用公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd`、esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d` 的独立 checkout，在这组源码与 WAMR fork 下完成 C3 原样样例构建，镜像大小 `0x39170` 字节、SHA-256 为 `d70de72efbdb9438299f3ce4d8d2877ab29f9fbf640a8f30fef148533e3bb204`；原样样例尚未强制链接或执行新私有宿主导入。

当前组件 SDK 锁升级至公开 ESP-IDF fork `578cf89c343e388db43ba1f4ddcd602fedcb763c`，直接承接上述旧提交并修复 HTTP 客户端初始化失败时的传输句柄泄漏；lwIP 和 WAMR 精确提交不变。`check_sdk.py` 核对独立 IDF/lwIP 检出通过；固定 wasi-sdk 33 的 Python unittest 18/18、普通主机 CTest 8/8 通过。新建构建目录下的 C3 原样样例完整编译链接通过，app 镜像 `0x39360` 字节，SHA-256 `af85f6251f2b571154eb6d3671d45079e93a86dd70f1a55425285640ebb50ba7`。该样例不调用 HTTP 客户端，也没有重新运行定时器 QEMU 或实板测试；此处只证明 Container 与新版 SDK 源码组合可构建。

此前在官方 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 加同一 lwIP/WAMR 的软件检查中，临时于样例 `main.c` 嵌入 690 字节测试 guest，仿真后恢复原文件；测试镜像 SHA-256 为 `2a6536da56db7466ff6eadbf9d77c08294dec8b82150c86fc40ac2f81583ac22`。官方 Espressif QEMU 9.2.2 的 C3 串口记录为 `host-api open=0 init=0 log=0 log_size=4 event=0 result=0 stop=0`，旧指令预算探针仍为 `normal=1 instruction_limit=1`。日志保存在仓外 `/tmp/esp-container-host-abi-qemu-final.log`；该运行结果属于原 SDK 基线，不能替代当前 fork SDK 的私有导入真运行。

## 未闭合边界

当前只有时钟、单条待取日志和实例私有定时器。消息、设备、业务命令与持久存储授权，其他异步操作的完成/取消，产品日志过滤、可能阻塞原生操作的可抢占取消和组合资源配额仍未实现。单次宿主期限的软件接线见文首，实板返回时限仍未验收。签名包设备验包、Flash 三槽、Base 装配、五组件同时运行的动态 RAM、真实 C3 板和失败恢复均未由本次测试证明。P6-04/P6-07 保持进行中。
