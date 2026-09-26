# ESP32-C3 FRP 与单页 guest 同存 RAM 条件下界：2026-09-27

## 结论与证据范围

在[当前精确五仓 QEMU 切片](five-component-qemu-capacity-probe.md)中，Base 报告 READY、一页 64 KiB ABI 2 guest 存活时，`MALLOC_CAP_8BIT` 空闲堆／最大连续块为 **65,480／45,056 字节**。此时没有配置 Wi-Fi，也没有 FRP session、TLS、FRPS、MQTT Broker 或 OTA 下载。真实 FRP reader 收到合法 64 KiB 记录的 12 字节 nonce 和 4 字节长度头后，在第 15 个 4 KiB 块申请处返回 `EFRP_NO_MEMORY`；14 块随后全部释放，记录没有进入完整认证。

基于**这一个无网络 QEMU 状态**和当前 ELF 的对象尺寸，完整 FRP 控制会话与满长记录同存，除现有 65,480 字节外，至少还需 **35,728 字节**可用的 8-bit RAM；若峰值后还按[五仓主计划](https://github.com/darren-you/darren-space/blob/master/harness/docs/design/darren-space/global/esp-base-frp-mqtt-ota-container-development-plan.md)保留 48 KiB 空闲堆初始观察门槛，下界为 **84,880 字节**。这是忽略多项实际成本的必要条件，绝非达到该数值即可验收的充分条件；实板联网峰值尚无测量。

本审计只读仓外镜像、map、QEMU 日志和精确提交源码；没有重新构建、修改源码、配置或设备。下界不含后续只替换 FRP 版本的实验结果，不跨锁合并内存读数。

## 精确输入与内存区域

五仓锁为 Base `058e965671fa0e4d417114897541710699571c52`、FRP `36e1506a2145321fc292294de59c0aa4532f73a7`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `207273188b984161362824c3344614e812016836`、Container `8eb805f3f12cb3cd836e9833acb4aca878ae80e7`；WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`，ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。完整归档与探针输入摘要由原 QEMU 收据保存。

只读文件来自 mac-work-1 的独立 `/private/tmp/esp32c3-five-dynamic-exact-20260927/probe/firmware/build/`：`esp_base.map` SHA-256 `69c574317afd11603563829d854621da1cca51635a072c4f3a37e5456de0124d`，`esp_base.elf` SHA-256 `d777068d145759e06e0fdd2ef21c35b65856fd9aaec5bbc55a434f7f855a796e`；成功 QEMU 日志 SHA-256 `e8bfe76270d5e75d92fbeb9e81c991b5dd3a78f5e29dc2bca0ff73476323afee`。QEMU 专用 ADC2 空桩使该镜像**不可刷实体板**。

当前 map 的 `.iram0.text` 为 **65,898** 字节，`.dram0.data` 为 **13,288** 字节，`.dram0.bss` 为 **91,592** 字节。QEMU 启动日志注册四段可动态分配的 RAM，长度依次为 91,200、116,496、10,576、8,132 字节，合计 226,404 字节；Base 初始化前的实测 8-bit free 是 201,516 字节，不能把 map 静态段与区域总长直接相减冒充运行时 heap。后一段 RTCRAM 已出现在启动注册记录中，不是未计入的额外空间。Espressif 说明 C3 的 IRAM 与 DRAM 共用片上 SRAM，静态 IRAM/DRAM 均影响可用堆；`MALLOC_CAP_32BIT` 可使用的部分 IRAM 只能按 32 位访问，现有 AEAD/Wasm 字节缓冲不能直接搬入。[ESP-IDF v6.1 内存类型](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/memory-types.html)、[堆能力与最大连续块](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/mem_alloc.html)

## 同存对象的必要空间

尺寸来自**上述同一 ELF**的 `riscv32-esp-elf-gdb -batch` 对 DWARF 类型执行 `p sizeof(...)`：`struct efrp_session`、`efrp_yamux_t`、`struct efrp_tls`、`struct efrp_client`、`struct efrp_port`、`struct efrp_connect`。FRP `36e1506` 的 [`src/client_port_idf.c`](https://github.com/esp-space/esp-frp/blob/36e1506a2145321fc292294de59c0aa4532f73a7/src/client_port_idf.c)明确另行申请 6,144 字节 worker 栈；[`src/aead.c`](https://github.com/esp-space/esp-frp/blob/36e1506a2145321fc292294de59c0aa4532f73a7/src/aead.c)在长度头通过后一次申请最多 16 个各 4,096 字节块，并在认证成功前不交付明文。

| 同时存活对象 | 字节 | 来源／边界 |
| --- | ---: | --- |
| session、Yamux、TLS 对象 | 17,848 + 5,552 + 2,456 = 25,856 | ELF DWARF；仅对象本身 |
| 满长 AEAD 的 16 个分块 | 65,536 | 64 KiB 明文；16 字节 tag 在 reader 对象内 |
| **session + TLS + 满记录小计** | **91,392** | 相对当前 65,480 free，至少缺 25,912 |
| client、port、connection、worker 栈 | 3,088 + 528 + 56 + 6,144 = 9,816 | 完整 FRP client 路径还须持有 |
| **完整路径的已知同存对象** | **101,208** | `101,208 - 65,480 = 35,728` 字节缺口 |
| 再保留 48 KiB free | 49,152 | `101,208 + 49,152 - 65,480 = 84,880` 字节缺口 |

登录用 4,096 字节 `handshake_rx` 在 `finish_login` 成功后清零释放，以上没有在控制记录阶段重复计入。反过来，表中**未计** client 自持的 CA 副本、Mbed TLS 证书／读写缓冲与 PSA、DNS/TCP/lwIP、FRP 工作流、Wi-Fi、MQTT、OTA、FreeRTOS/allocator 元数据和碎片。当前 `sdkconfig` 的 TLS 输入／输出内容长度为 16,384／4,096 字节、动态缓冲关闭；这些配置值不是实际同时分配量，不能直接与表中尺寸相加。guest 自身已包含在 65,480 字节读数之前，不能再次从表中减去，也不能缩小 ABI 2 的标准 64 KiB 页来填平差额。

## 静态缓冲不能直接兑换为并发余量

同一 map 用本仓 [`map_first_party_dram.py`](map_first_party_dram.py)筛第一方设备地址范围 `.bss/.data`，最大的十个 BSS 对象合计 **65,470 字节**。其中 `s_load_bytes` 和 `s_commit_bytes` 各 7,618 字节、解析命令 `command` 8,400 字节、串口 `s_reader` 9,228 字节；即使假设这四项能在满记录期间**全部**释放且 1:1 变成可分配堆，也仅回收 **32,864 字节**，仍低于 35,728 字节的乐观最低缺口，更未触及 TLS／网络及 48 KiB 余量。

实际生命周期也不支持直接删除：配置提交须保留编码候选与读回比较，`s_context.config` 仍是当前配置、`s_candidate` 在异步应用期间保留待提交配置；`s_reader` 需跨轮次保存部分串口行，`command` 还须解析后续命令。MQTT 的 7,716 字节 `s_work` 已将配置暂存与事件暂存做 union 复用。改成临时堆申请只会把相关命令的峰值移到 heap，并可能在满记录同存时造成控制路径申请失败；需要按真实并发和恢复合同另行证明，不能把静态 map 缩小当成已增加相同的运行余量。

现有分块已移除单笔 65,552 字节连续申请，但并未降低满记录累计 65,536 字节需求。当前 45,056 字节最大连续块足以说明单笔 4 KiB 申请并非唯一问题；它不能保证 16 块、session 和 TLS 在碎片化堆中共同成功。保留 64 KiB FRP wire、完整认证前不交付明文和完整一页 guest 的前提下，本轮没有找到可由上述静态清单直接证明的低复杂度闭环。P6-03 仍需在同一镜像和实际网络负载中记录 heap、最大连续块、各任务栈及分配失败，再由实板分别验证。
