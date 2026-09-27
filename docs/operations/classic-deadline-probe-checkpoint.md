# 双目标 Classic 期限独立探针检查点

2026-09-27，在 Container `6ef74faabb675bce0180570f5bdf0232af11106a` 的独立分支上扩展 C3／ESP32 共用最小探针。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。本轮只构建样例并在 mac-work-1 的 QEMU 合成 Flash 中运行，未连接或写入实体设备。

共用源码 `examples/runtime-probe/main.c` SHA-256 为 `48d5fe994a08f3ef908c634301e02cf89120aa3a7eb4ad8ad9b345af3b45fbca`。正常模块返回 0；同一无限循环 Wasm 分别在 1000 条指令额度与 `INT32_MAX` 指令额度加 20 ms 绝对单调期限下执行。后者只有精确返回 `Exception: wall clock deadline exceeded` 才判通过。每轮新建可写 Wasm 副本、实例和执行环境，并在调用后清除期限；WAMR loader 可能改写传入字节，不能把一次加载后的原缓冲区再用于静态扫描。

首次 ESP32 QEMU 在指令额度测试后重用同一静态 Wasm 数组，第二轮得到 `module scan rejected looping`，最终 `wall_clock_deadline=0`；失败日志 SHA-256 `9d7a71aff6494b4195b6340f849b6faaa39640045e24a6156440e08a063c8b57`。为每轮分配、复制并在模块卸载后释放输入后，双目标完整构建和仿真通过。原始构建、镜像、日志、配置与摘要索引位于 `mac-work-1:/private/tmp/esp-container-deadline-board-probe-20260927/` 的 `public-evidence-summary.json`；没有读取私有设备 Flash。

| 构建目标 | app 大小／SHA-256 | 配置与编译核对 | QEMU 结果 |
| --- | --- | --- | --- |
| C3 原生 USB 样例 | 220,112 B；`8bb65ed21ac54d6be3a9738ea42a598f656c3a8ea58309f114fee2cb53ef7ae4` | 最终控制台为 USB Serial/JTAG；样例和 `wasm_interp_classic.c` 均带期限宏 | 原生 USB 日志不由该 QEMU UART0 回传，未把此镜像算作运行证据 |
| C3 仓外 UART0 变体 | 221,104 B；`ce5dbe48047f6928584e88d4f4bcb0d6ece9bec914e3e79b96767d94be466ac9` | 只在仓外把样例控制台改为 UART0；依赖锁 SHA-256 与原生 USB 样例同为 `fcd76251f325b0a4f851abfcc1bfcebe050d45eed495a87942beb7133dde1e2e` | `normal=1 instruction_limit=1 wall_clock_deadline=1`；期限一轮约 20,021 µs；前后 8-bit free/largest 326,688/188,416 → 326,584/188,416 B。日志 SHA-256 `a5aeb6a49d0b25eaaa8c969f29df84bf89857d574ca382fb8727ae50f5209e48` |
| ESP32 UART0 样例 | 212,384 B；`fb4ddc022f23186c12cd200998269b996821a1f6122d5a467c9a196a9dc927b6` | 最终控制台为 UART0；样例和 `wasm_interp_classic.c` 均带期限宏；锁 SHA-256 `aa16e5cd4cb8514e27e7a2b6939ad97be72cffa71591035a1b6ea8a56e3e770e` | 同样三项均为 1；期限一轮约 20,031 µs；前后 free/largest 294,768/163,840 → 294,660/163,840 B。日志 SHA-256 `1c8b658d46383798dfecfb9965a253aa2db28bd86b296a264aac202fcce1d3aa` |

这些历时只是两次仿真观察，不构成 20 ms 硬返回上界。样例直接调用 WAMR API，未装载签名业务包或运行 Container `product_*` 三入口；QEMU 也没有物理 Flash、Wi-Fi、Broker、FRPS、OTA 或设备调度。C3 UART0 变体不适用于只有原生 USB 控制台的 C3 实板。两份样例自带单 app 分区表；写板前必须按五仓计划第 13.1 节重新确认身份、当前分区与两份新鲜全片恢复件，不能直接使用样例 `flash_args`。P6-02 的新锁双板最小运行与 P6-07 的设备期限验收均未由本轮 QEMU 关闭。
