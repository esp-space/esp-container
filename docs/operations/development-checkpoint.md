# 开发检查点

## 2026-09-30 Go 主机验包 SDK

`host/` 新增独立 Go module，`productpkg` 仅依赖标准库，严格复用现有 v1 包格式、独立 RSA-3072/SPKI 信任锚、固定 PSS 参数与 Classic ABI 2 静态合同。规范数字、重复／未知字段、完整 ID／版本、清单与 Wasm 上限逐项检查；所有验证失败返回零值，不暴露半验证元数据。SDK 不执行 guest，设备平台授权、候选 Flash 回读与试运行仍由实际设备完成。

Go 单测／race／vet 通过；固定公开签名向量、独立进程内重签名负例、错误公钥／key ID／PSS salt、3000 字节完整版本、输入归属和失败零值通过。Python／浏览器／Go 对同一组 37 个独立签名输入逐项互验，6 个合法输入均接受，31 个负例均拒绝，完整清单／包 SHA／长度／实际 SPKI 指纹一致；包含清单 4096／4097 字节、uint32 最大 schema、Wasm 512 KiB 边界。Python 全量入口改为 `-p '*test*.py'`，实际运行 18 项，另有需要 wasi-sdk 的 guest 构建条件跳过；Node 7/7 通过。包解析与 Wasm reader 的两次 15 秒 fuzz 分别完成 8761／1075898 次执行，无 panic 或失败元数据泄露。独立 module 源归档、空 module/build cache 的测试通过；Go 版本为本机 1.26.1，module 声明最低 1.25。

本轮只改主机 SDK、测试与文档，未改设备 C API、固件锁、生产信任锚或设备存储。完整制品持久登记、平台授权交付及双板实物仍待计划任务闭合。Tool 将通过精确 Go module 提交消费，不复制解析代码；浏览器既有 SDK pin 与 Base 的 C 依赖保持各自已验证版本。以下保留此前设备实现检查点。

## 2026-09-30 实际装载实例的签名清单元数据

公开 `econtainer_product_open` 在选中包重新验签／授权且 WAMR 实例创建成功后，现返回产品 ID／完整版本在调用方清单工作区中的切片，以及 guest ABI／data schema。只有 slots／runtime 均 OK 且实例非空时填充；存储拒绝、映射解除失败、授权、全局 BUSY 和真实引擎失败均保持六字段为零。切片不引用已解除的 Flash 映射，调用方可在工作区再次被修改前复制；组件没有新增 heap 缓存、重复验包或 64 字节版本限制。版本保持既有 4096 字节整份清单上限。

独立 Git 归档加本次已跟踪源文件差异，在固定 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`／wasi-sdk 33 上完成普通和 ASan/UBSan 全量 CTest，各 9/9 通过。真实签名 P0→P3 的确认、trial 和旧包恢复逐项比较产品 ID／版本／ABI／schema；P1 使用 3000 字节版本名，预期字节由签名输入单独提供，并按整包摘要匹配实际选择。测试先确认映射已解除、槽锁已释放，再从调用方工作区读取完整切片；已有单实例、并发槽写、指令／期限、原生资源与百次生命周期回归保持通过。

同一独立源码的 132 个已跟踪非文档输入逐项与候选一致。固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与唯一 lwIP 合同检查通过，C3／ESP32 独立运行样例完整构建；两目标均核对新 `product.c`／`slot_runtime.c` 实际对象、WAMR 精确锁和目标控制台。样例不调用产品装载入口，故编译对象不等于最终 app 深链接或完整 Base 资源通过。

| 目标 | 独立样例 app 字节数 | app SHA-256 |
| --- | --- | --- |
| ESP32-C3 | 220,112 | `f8f42924e5fdebaa0edac6c40317397ca747e8565351734e9e977f671a0894b9` |
| ESP32-D0WD-V3 | 212,384 | `47ba52b5c98faaa516ddfdcc73c32dab8baa47e1b92d9361ddb08026587209b2` |

独立输入、配置／构建、CTest 和摘要回执保留在仓外临时目录。首轮验证脚本在 macOS Bash 3.2 的空数组展开处退出，尚未开始 CMake；修正验证脚本后执行上述完整检查，组件源码没有为此修改。当前仅 C3 连接，本轮未访问实体串口、复位、Flash 或 eFuse，未使用生产密钥。Base 与 Tool 尚未消费本次新增结果，实际活动版本／trial 回读和界面仍待接线；物理迁移、真实网络、五能力峰值与完整 P8-04 不据此验收。当前 API 合同见[公开产品生命周期](public-product-lifecycle-checkpoint.md)。

## 2026-09-29 物理 I/O 租约释放失败闭合

IDF provider 的 `release_flash_io` 改为返回成功与否：NVS blob 与包 Flash 的读、擦、写在释放失败时均报告 I/O 失败，即使底层写入可能已经生效；调用方须从持久事实重新对账。映射解除后若租约释放失败，公开装载入口关闭刚创建的 WAMR runtime 并返回 `IO_FAILED`，不进入 guest。真实 WAMR 主机 CTest 9/9 通过，故障注入覆盖这些回调和装载路径。该结果只验证合成 SDK 与主机解释器，实板 Flash/NVS 故障和最长映射占用尚未测量。

## 2026-09-27 ESP32 五组件完整认证记录 QEMU 容量探针

以 Base `1f43b6f`、FRP `1f0c8f3`、MQTT `9d6d95e`、OTA `2072731`、Container `8eb805f`、WAMR `26c235e` 和固定 ESP-IDF／lwIP 装配仓外 ESP32 镜像，临时 ECDSA v1 测试键签名文件 **1,114,100 B**、验签数据长 1,114,032 B，官方验签与 `0x120000` app 槽容量检查均通过。官方 Xtensa QEMU 两次运行在 Base READY 后得到 free／最大块 **147,340／110,592 B**；单页 ABI 2 guest 存活时为 **61,404／43,008 B**，两次完整 4 KiB AEAD 认证和逐字节比较通过，坏 tag 拒绝且无明文，完整 64 KiB 记录在第 14 块申请失败并清理。Base 初始化前同一 guest 与完整 64 KiB 记录则认证成功。探针无 ADC/Wi-Fi 空桩，但无真实 Wi-Fi／TLS／Broker／FRPS／OTA 及实板并发，P6-03 仍未验收。完整源码锁、trace、首次清理差额与复现命令见[独立报告](esp32-authenticated-qemu-capacity-probe.md)。未接设备或使用生产密钥。

## 2026-09-26 双目标独立运行探针

在 `codex/c3-low-memory@0024695510e2b445bfdb089247d31f8cbd84e010` 的未提交候选上，C3 与 ESP32 样例共用 `examples/runtime-probe/main.c`，分别保留自己的 target、控制台和 `dependencies.lock`。在 mac-work-1 的仓外副本使用锁定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 与 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5` 执行 `check_sdk.py`，再分别运行 `idf.py -C examples/c3-runtime build` 与 `idf.py -C examples/esp32-runtime build`，均完整链接。生成锁分别声明 `target: esp32c3`、`target: esp32`；ESP32 `sdkconfig` 确认为 UART0 控制台与 4 MiB Flash。

初轮 C3 UART0 app 为 232368 字节，SHA-256 `5fef4fdfe55d74bd768802a6df7e9d862016e05959d0a89a934f75c4c0afb63c`；ESP32 app 为 217088 字节，SHA-256 `b23bc1c47d21291c3091d9a4cd4c35917395b3793f6320f0c2fcc4af9990802a`。两份 `compile_commands.json` 均确认唯一探针源码是 `examples/runtime-probe/main.c`，分别由 RISC-V 与 Xtensa 编译器处理。mac-ci-1 的 Python 14 项通过，`WASI_SDK_ROOT` 未设置导致 counter 编译用例跳过。mac-work-1 的官方 QEMU RISC-V 9.2.2 (`esp_develop_9.2.2_20260417`) 执行初轮 C3 UART0 镜像，正常调用返回 0，死循环得到精确指令额度异常，最终 `normal=1 instruction_limit=1`；运行前 free/largest 为 326416/188416 字节，运行后为 326312/188416 字节，无 panic。本机现有 Xtensa QEMU 没有 ESP32 机器，因此 ESP32 仅完成编译。上述软件结果不能证明两块实板的真实 WAMR 执行、堆峰值、五组件组合或新包槽布局，P6-02/P6-03 保持进行中。

同日实板前置复核发现，上述 C3 构建最终 `sdkconfig` 实际选择 UART0 主控制台，只能作为 UART0 QEMU 参考，不作为只有原生 USB Serial/JTAG 端点的 C3 板候选。随后在本分支的 C3 `sdkconfig.defaults` 显式设置 USB Serial/JTAG 主控制台与无 secondary，移走旧生成 `sdkconfig`/`build` 后使用同一固定 SDK/WAMR 重新完整构建。新配置读回 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`、`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`、`CONFIG_ESP_CONSOLE_UART_NUM=-1`，仍为 `esp32c3` 与 4 MiB；`dependencies.lock` 固定 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`。新 app 为 **227616 字节**，SHA-256 `182ede7194b65ccf18630b53ab845c8039f57306f686a2deb7f5a0ad278e003b`；bootloader 与分区表 SHA-256 分别为 `afe114c7d997b4af12849fb0e370b67e4a8888a12bb7a2f657dda9b2319eaee8`、`7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820`。app、bootloader、分区表、两种 flash 参数文件、最终配置与锁已传到 `mac-pro-1` 的私有 `container-c3-20260926` receipt，逐项摘要校验通过；传输本身没有设备写入。样例 `flash_args` 是单 app `0x10000` 布局，不能直接用于现有 Base 双 OTA 分区板。

对新 USB 主控制台镜像执行官方 C3 QEMU 时，`-serial mon:stdio` 只见 ROM 引导、没有探针串口行，不能把它记成 WAMR 调用通过；日志 SHA-256 为 `483226d55bbdbfce11889db13ac34ae13af02f04d1947e93d3a786e2f5220b9e`。将上述仅差控制台配置的旧 UART0 参考镜像在同一 QEMU 再运行，正常返回和精确指令超额异常仍通过，`normal=1 instruction_limit=1`，运行前后 free/largest 为 326416/188416 与 326312/188416 字节；日志 SHA-256 为 `109eb1b0c04436cff77a7420effd014ec94493f84c976a9f6f9bb4a999e54789`。USB 实板的 WAMR 与资源结果须以目标端点实际串口日志另行裁决。

### C3 原生 USB 实板最小探针

同日于 mac-pro-1 对 P1-04 已登记的 ESP32-C3 rev0.4／4 MiB 板重新核对芯片、MAC、唯一 USB Serial/JTAG 端点、原 Base 持久身份、配置 revision 5 与当前 `ota_0@0x20000/0x1e0000`、`otadata` 的 VALID 选择。写前重新读取两份完整 4 MiB Flash，两份彼此及 P1-04 旧恢复件逐字节一致。使用上述 USB 主控制台 app `182ede7194b65ccf18630b53ab845c8039f57306f686a2deb7f5a0ad278e003b`，仅写当前 `ota_0` 的 227616 字节；esptool 写后校验与独立应用区读回均逐字节一致。没有写样例自己的 bootloader、单 app 分区表、NVS 或 eFuse，也没有操作另一块板。

原生 USB 串口记录 `before free/largest=326652/188416`、正常 `run call_ok=1 result=0 exception=none`、死循环 `looping call_ok=0 ... Exception: instruction limit exceeded`、`after free/largest=326548/188416`，最终 `normal=1 instruction_limit=1`，未见 panic 或 WDT。该采样是最小探针运行前后资源值，不是执行期间峰值或完整五组件容量。

实验结束从本轮恢复件全片写回并由 esptool 校验，再独立读回完整 4 MiB；读回与写前两份以及 P1-04 两份恢复件逐字节一致。复位后原 Base `status` 成功，持久身份、revision 5、config ready 与管理能力同前，boot ID 按重启更新；Wi-Fi 曾短暂处于 connecting，下一次查询回到原 disconnected。原始 Flash、MAC、UUID 和设备日志仅保存在 ESP Tool 私有 `container-c3-20260926` receipt。P6-03 的五组件负载与新包槽布局仍未验收。

### ESP32-D0WD-V3 UART0 实板最小探针

同日仍在 mac-pro-1，对 P1-04 已登记的另一块 ESP32-D0WD-V3 rev3.1／4 MiB 板重新核对芯片、MAC、CH340 UART 端点、原 ESP-AT `1.1.b1.0` 的 `AT+GMR`、`AT+CWMODE?` 与 `AT+CWJAP?` 只读响应，以及旧 `ota_0@0x100000/0x180000`、擦除态 `otadata` 和旧 bootloader。P1-04 旧两份完整 Flash 恢复件一致；本轮初次尝试的第二份读取因 CH340 stub 对 flash 命令报错，未写入设备，重新进入下载模式后原 AT 仍响应。改为每次明确复位进下载模式，取得本轮两份各 4 MiB、逐字节一致的完整 Flash。两份与旧归档只在原 NVS 有运行期变化，恢复使用本轮双份，不复用 C3 恢复件。

同锁 ESP32 UART0 app 为 **217088 字节**，SHA-256 `b23bc1c47d21291c3091d9a4cd4c35917395b3793f6320f0c2fcc4af9990802a`；实际 `sdkconfig` 为 `esp32`、4 MiB、UART0，镜像 chip ID 0、校验和与验证哈希有效。仅写当前旧 `ota_0` 中的 app 字节，esptool 写后校验及应用区独立读回逐字节通过。没有使用样例 `flash_args` 的单 app `0x10000` 地址，也没有写样例 bootloader、分区表、NVS、eFuse 或另一块板。2017 年旧 AT bootloader 在此次受控实验中实际进入 SPI Flash 启动并运行该 IDF 6.1 app；这只证明此独立样例的实际启动，不建立新签名平台的启动基线。

UART0 原始日志记录 `before free/largest=294688/163840`、正常 `run call_ok=1 result=0 exception=none`、死循环 `looping call_ok=0 ... Exception: instruction limit exceeded`、`after free/largest=294580/163840`，最终 `normal=1 instruction_limit=1`，未见 panic 或 WDT。实验结束以本轮完整恢复件写回并经 esptool 校验，再独立读回 4 MiB，与本轮两份恢复件逐字节一致；原 AT 版本仍为 `1.1.b1.0`，配置只读查询均成功且结果逐字节等于写前。原始 Flash、MAC、AT 配置和串口日志仅保存在 ESP Tool 私有 `container-esp32-20260926` receipt。

两个 target 的同一共享探针源码均已在真实板上验证最小正常调用与精确指令额度异常。本轮构建发生在 `codex/c3-low-memory@0024695510e2b445bfdb089247d31f8cbd84e010` 的未提交工作树；随后把参与构建的共享源码、双目标 CMake／默认配置和锁保存为本地提交 `fc1d3bec72c1604e406126eabe7c6a098fdb6785`，其余修改为本轮证据文档。共用 `main.c` 的 SHA-256 为 `7fd91d4e03e4989a2cf79e9f7b8968cf6d2cb4ffaad12773c969effc33ba2fbd`，两目标生成锁均固定 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`；mac-work-1 所用 RISC-V 和 Xtensa 编译器目录版本均为 `esp-15.2.0_20251204`，IDF/lwIP 精确源码见页首。独立 guest SDK 输入仍为 wasi-sdk 33、官方 macOS arm64 资产 SHA-256 `85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78`；本段最小探针直接内嵌两段固定 Wasm 字节，没有调用 guest 编译器。该提交未推送，不把真板结果自动赋予后续源码改动。两板的 Flash／堆数字只属于独立探针，不证明 P6-03 的五组件并发、真实业务包、三包槽或新分区。

## 历史检查点

当前 C3 分支的 ABI 2 页内事件区、公开 WAMR `26c235e` 与最新验证见[单页 profile 检查点](c3-low-memory-profile.md#abi-2-页内事件区与标准页边界)。以下日期与版本记录保留各自历史范围。

### 已核对的软件范围

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| `python3 -m unittest discover -s tests -v` | 9 项通过 | 主机 RSA-3072/PSS、规范 JSON、受限 ustar、Wasm 初筛；测试键仅在临时目录产生 |
| CMake/CTest `wasm_scan` | 1 项通过 | 初始无 import/no start 扫描，不是完整 guest ABI 或设备验包 |
| `darren-you/esp-container` 公开 `master` 独立 checkout | 从首个第一方提交 `756244f18d66115eb17524707bf558063ef8216f` 重新拉取，Python 9 项与 CTest 1 项均通过 | 只证明公开仓主机工具可复现；不代表设备运行链路 |
| 固定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643` + lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 构建 `examples/c3-runtime` | 原样官方 WAMR `8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629` 失败 | 编译 `espidf_platform.c`、`espidf_file.c` 缺 POSIX stat 类型，以及 `espidf_memmap.c` 引用当前 C3 内存保护下不存在的 `MALLOC_CAP_EXEC`；这是修复前的失败基线 |
| 公开 WAMR fork `a34d721b630213f59fde0b40cebbb980903660e8` + 当前 `esp-container` 组件清单与锁 | 固定 IDF/C3 全部编译链接通过；bin 227168 字节，SHA-256 `092e3fc1d1df33e3c98859e34167098ddeb4815d19148ff117fbd758e9b8975f` | 锁文件精确指向公开 fork；`CONFIG_ESP_SYSTEM_MEMPROT=y`，35 个 WAMR 编译单元不含 `espidf_file.c`，计量与 Classic 编译标志已核对；未刷板、尚无实际运行/预算异常或堆峰值 |
| 精确 WAMR fork 的主机 Classic 执行与 C3 探针异常判定收紧 | 锁文件 WAMR `a34d721...`、`.component_hash=e7a23b...` 一致；主机 CTest 2/2 通过，正常函数 `call_ok=1/result=0`，死循环 `call_ok=0/Exception: instruction limit exceeded`；固定 SDK/C3 重新完整链接，bin 227232 字节，SHA-256 `b78ce257e17ef505a20c8c8b0f2af550ca3e1fa897494faabab5dabd5b55b8e1` | 这是本机真实解释器执行与 C3 构建证据；尚未刷板，不能声称 C3 实际执行、堆峰值或全负载预算通过 |
| 公开 `master@e27b5dc5cdb5979c7d83dc09833d21643604498c` 全新 checkout | 独立目录 `/tmp/esp-container-public-e27` 从 GitHub 克隆；Python 9/9、真实 WAMR host CTest 2/2、固定 SDK/C3 完整链接均通过；锁文件和下载组件 `.component_hash` 同为 `e7a23b...`，bin 227232 字节，SHA-256 `effcfec54778d734b2bd1b12a3242fdfeeccb7db6db184bca044e3879eb92f65` | 两次 C3 构建路径不同，SHA 不相等；不宣称逐字节可复现，也不以主机结果替代实板异常与堆峰值 |
| `esp-container` 公开 `master@ae52a774857eb7839481f012dcff03c8a22d1e3f` 全新 checkout | Python 9 项、CTest 1 项通过；固定 SDK/C3 全部编译链接通过，bin 227168 字节、SHA-256 `99e05d6e7e544a7978a912f07a0862f0d0e60e514c456382cdfbeb39f890df00` | 与本机原 checkout 镜像尺寸相同；构建时间等非源码输入会改变摘要，未声明位级可复现；未写板 |
| wasi-sdk 33 freestanding counter guest | 官方 macOS arm64 资产 SHA-256 `85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78`；`WASI_SDK_ROOT` 下 Python 13/13，counter 437 字节、SHA-256 `1896ae9ed2390bd4fb71af9d50965533a679245806c9c07675d65b81be42c91c`；两种输出路径字节相同，静态 ABI/profile 检查通过，锁定 WAMR 主机实际调用 init/event/stop 通过 | 只验证 counter 样例的编译、输出形状和主机 Classic 执行；无实板、宿主授权、句柄、包安装或完整 P6-04 结论 |
| 五组件仓外 C3 容量原型 | [独立报告](five-component-capacity-probe.md)：固定五仓及 SDK/WAMR 提交的实际 ELF/map 保留 FRP TLS/Yamux、MQTT 发布订阅、OTA HTTPS 与 WAMR Classic 调用；RSA-3072 签名镜像 `0x121000`，官方分区工具验证两组 4 MiB 布局 | P6-03 未验收：Base 旧 MQTT 试验组件仍冲突；极限布局双 app 零余量且 512 KiB Wasm 打包超槽；留余量布局尚无真实业务包、动态 RAM、迁移和实板数据 |
| 五组件仓外 S3 静态探针 | [独立报告](s3-five-component-static-probe.md)：当前五仓/固定 SDK 在临时 S3/8 MiB Flash/Octal PSRAM 配置下强制链接成功；双 2 MiB app、3 MiB 包分区及 128 KiB Base NVS 几何通过，测试键签名镜像 `0x111000` 并通过 RSA 验证 | 硬件未选定，Base/FRP 仍有 C3 目标语义；没有 S3 运行、RAM/Flash 并发、真实安装或实板数据，不支持 S3 发布，P6-03 未验收 |
| 官方 Espressif QEMU ESP32-C3 最小运行 | 公开 `4a37af82b44a8a4e4444a64652cee6c6e29e08eb` 的 `xTaskCreate` 探针仿真时在 `pthread_self` 断言；后续 `f013c6a8ae60e4232f1815782e72e8b174d9f790` 仍保留该入口。改用与锁定 WAMR ESP-IDF 示例一致的 joinable pthread 后，固定 IDF/lwIP 与官方 `qemu-riscv32` 9.2.2 (`esp_develop_9.2.2_20260417`) 仿真启动成功。真实 WAMR C3 日志：`before free=326416 largest=188416`；`run call_ok=1 result=0 exception=none`；`looping call_ok=0 ... Exception: instruction limit exceeded`；`after free=326312 largest=188416`；`normal=1 instruction_limit=1`。Python 9 项通过、counter 编译测试因未设置 `WASI_SDK_ROOT` 跳过，固定 WAMR 主机 CTest 2/2 通过；C3 bin 231488 字节，SHA-256 `2e93e4f42debb306224f7550929fe2cf9b67d110cfed82e35e80e8d04cd9115f` | 仅为 QEMU 仿真与本机软件证据；样例没有 Wi-Fi、FRP、MQTT、OTA、生产包和真实 Flash 布局。未刷板，不是实板堆峰值、时限或掉电行为验收 |
| [组件私有单实例运行切片](single-instance-runtime-checkpoint.md) | 固定 wasi-sdk 33 + 锁定 WAMR host CTest 3/3；counter、事件复制与分配失败、ABI 和原始内存页拒绝、init/event/stop 各自额度异常、关闭重开通过。固定 IDF/C3 组件源码编译、原样样例链接与仓外私有 API 强制引用链接通过 | 原样样例没有调用新 API；本项仍缺 C3 新链路真运行、签名包、宿主能力、Base 装配、RAM/实板结果，P6-04/P6-07 未验收 |

公开 fork 从官方 WAMR-2.4.4 精确提交直接修正三处源码：无 WASI 时不编译文件适配、明确包含 `<sys/stat.h>`、在 IDF 无可执行堆能力时拒绝执行映射并保留普通映射。构建保留 `CONFIG_ESP_SYSTEM_MEMPROT=y`，未定义虚假的 `MALLOC_CAP_EXEC`，也未修改固定 SDK 或 `managed_components`。上述为历史公开提交的检查点；当前双目标实板探针及新的 WAMR 锁以本文首节为准。

当前已有只读组合链接与分区几何证据，但仍无实际 Flash 三包槽、设备流式验包、产品实例管理、真实签名包运行、C3 堆峰值和分区迁移。主计划 P6-02 的完整源码冻结与配置合同复核，以及后续运行、容量任务仍未验收。
