# C3 空闲 FRP client 与完整认证记录同存 QEMU 容量探针

## 范围与输入

本实验只在 mac-work-1 的仓外 `/private/tmp/esp-c3-client-qemu-20260927/` 派生两套 ESP32-C3 工程。在 Base 报告 READY、一个 ABI 2 标准 64 KiB guest 保持存活时，调用真实 `esp-frp` 公共 API `efrp_create`、`efrp_get_status` 和 `efrp_destroy`，并在 client 尚存活时由**独立手工 AEAD reader 探针**送入一条完整 64 KiB 密文记录。**从未调用 `efrp_start`**；worker 状态为 `EFRP_PHASE_STOPPED=0`，没有 FRP session、TLS 握手、DNS/TCP、FRPS、Wi-Fi 数据会话、MQTT Broker 或 OTA 下载。`.invalid` 域名、本地回环地址、占位 CA／Token 都只是 RAM 中的测试参数，不能视为有效认证材料。

两套输入来自此前的[C3 新 FRP QEMU](five-component-qemu-capacity-probe.md)与[C3 Wi-Fi IRAM 两项开关对照](c3-wifi-iram-qemu-ab.md)。精确源码为 Base `058e965671fa0e4d417114897541710699571c52`、FRP `1f0c8f37db3765a74b3b95871bb266d0c73d1248`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `207273188b984161362824c3344614e812016836`、Container `8eb805f3f12cb3cd836e9833acb4aca878ae80e7`、ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`。构建使用原锁定 `esp_frp` 本地覆盖；生成 `dependencies.lock` 只记录本地路径，不能单独证明该组件提交，完整来源按上述原始冻结输入收据核对。

[准备脚本](prepare_c3_frp_client_qemu.py)先校验两套已有签名 app、最终 `sdkconfig` 及原始运行探针的 SHA-256，再复制到新目录，只添加[client 探针 C](frp_client_qemu_probe.c)、把它加入 CMake，并在原有满长记录后插入 client 创建、client 存活时的满长记录、销毁三个调用。原始 4 KiB、坏 tag 和无 client 的 64 KiB 记录仍按原顺序运行。两套**构建后**工程排除 `build` 与日志的逐文件对比，唯一不同文件为 `firmware/sdkconfig`；该文件只在 `ESP_WIFI_IRAM_OPT`、`ESP_WIFI_RX_IRAM_OPT` 及各自两处自动镜像别名共六行有差异。测试键、guest、wire、探针源码及组件均相同。

最终公开准备脚本 SHA-256 为 `93de34345f34a7a7d8b1eb6c2a483207118e48b39f9f2d9b16c23d11f48b58d5`，client 探针 C 为 `429e4340848e8465765293970777b35e3d0fad28dbde535548578bf5003d0e68`。最终脚本重新从两份冻结输入派生新目录时，生成运行探针 C 两侧均为 `d8e8d8ef83653dd7b5816e23e841581f857b5c53113e65164eab378af7e05c56`，client C 两侧均与上值相同；该重放只核对源码，没有替代下文两次实际构建和 QEMU 日志。

## 动态读数

以下 `free/largest/min` 均为 `MALLOC_CAP_8BIT` 字节；`min` 是开机以来的历史低水位，不能单独当作某一行的瞬时占用或作为两套配置之间的直接差值。`reader` 的 `during` 是 `efrp_aead_feed` 返回后、完成明文消费和显式 destroy 前的采样；失败路径已经自动释放块，所以失败行的瞬时峰值只能由已成功分配块数与低水位间接判断。

| 阶段 | 两项 Wi-Fi IRAM 开启 | 两项 Wi-Fi IRAM 关闭 |
| --- | ---: | ---: |
| Base READY + guest，原有记录测试前 | 65,480 / 45,056 / 56,448 | 84,944 / 45,056 / 55,428 |
| 无 client 的原有满长记录 | 第 15 块申请失败；消费 57,360 B；14 块释放 | tag 认证、逐字节核对 65,536 B；16 块释放 |
| 创建 client 前，原有记录已清理 | 65,480 / 45,056 / 7,248 | 84,944 / 45,056 / 18,528 |
| `efrp_create` + `efrp_get_status` 成功；worker STOPPED | **55,428 / 45,056 / 5,968** | **75,020 / 45,056 / 18,528** |
| client 占用的实测 free 净减 | 10,052 B | 9,924 B |
| client 存活时手工送满长 wire | 第 13 块申请失败 `EFRP_NO_MEMORY=-20`；消费 49,168 B，12 块全部释放；无完整 tag 认证 | `feed=0`、`records=1`、tag 认证且逐字节核对 65,536 B；16 块申请与释放 |
| 该 reader 返回后、显式销毁前 `free/largest` | 55,428 / 45,056 B；失败后已回滚 | **9,420 / 3,584 B**；完整记录仍持有 16 块 |
| reader 清理后、client 仍存活 `free/largest/min` | 55,428 / 45,056 / 5,968 | 75,020 / 45,056 / 6,228 |
| `efrp_destroy` 成功、guest 仍存活 | **65,480 / 45,056 / 5,968** | **84,944 / 45,056 / 6,228** |

两套 `probe_summary runs=2 failures=0`，第二次 guest 的 `open/init/event/stop/close` 全为 0，counter 返回 3。坏 tag 仍由原探针返回 `EFRP_AUTHENTICATION_FAILED=-11`，不交付明文并释放唯一块。无 client 及有 client 的 reader 均在各自阶段清理后回到进入前 `free/largest`。两次 QEMU 都在 45 秒后由宿主 SIGTERM 结束，未见 panic。

同一新 ELF 的 DWARF 给出 `sizeof(struct efrp_client)=3,088`、`sizeof(struct efrp_port)=528` 字节；锁定 FRP `client_port_idf.c` 确实为空闲 worker 申请 **6,144 字节**栈，三项确定的同时占用是 **9,760 字节**。`efrp_create` 还复制 19 字节占位 CA；实测堆净减另含分配器与 FreeRTOS 等开销。QEMU 仿真经 IDF `xTaskGetHandle("esp_frp")` 找到唯一 worker，`uxTaskGetStackHighWaterMark` 在创建后和销毁前均为 **5,720 B 剩余**，说明这个**未 start**阶段至多触及约 **424 B** 栈；6,144 B 仍需完整保留，不能据此缩小 worker 栈或推断 TLS/session 路径的栈峰值。

## 判断与边界

关闭两项 Wi-Fi IRAM 的实验配置让**空闲 client + 手工满长 AEAD reader + guest**在此 QEMU 镜像里同存，认证期间瞬时仅剩 9,420 B 可分配、最大连续块仅 3,584 B，历史低水位到 6,228 B。真实 `efrp_start` 后还会创建连接、TLS、session、Yamux 和工作流；[当前同存内存下界](dual-target-frp-guest-memory-bound.md)中的 101,208 B 不因为这个空闲 client 探针而减小。因此这项成功**不代表 FRP 会话已能承载满长记录**，也不支持把 Wi-Fi IRAM 开关改成产品默认值；性能和联网并发未测。开启配置更早在第 13 块失败。P6-03 继续进行中。

两个镜像均包含公开测试 RSA v2 签名键、UART0 QEMU 控制台和 ADC2 校准空桩，**绝不能刷实体板**。当前固定 SDK 的 `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=y`、`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y` 下不声明 `MALLOC_CAP_EXEC`，本实验没有把未测的可执行 IRAM 池算进可用 8-bit 堆。本实验没有改产品源码、默认配置、分区或任何物理设备。

## 构建、签名与复现

| 收据 | IRAM 开启 | IRAM 关闭 |
| --- | --- | --- |
| 输入签名 app SHA-256 | `93fb2b027f5bf8d0acae828e4812cd5665acc2803f21621ea2b7ee169051efdc` | `4f1935a3898172cb3983600572e6808f11f9a3426783bd393ceeec45a258f059` |
| 输入／最终 `sdkconfig` SHA-256 | `cb4911792bf9fc1191e4dfc90ff04e483e880ed6800a455f590e594c5ce6b62d` | `230e7a60b88a59e98b4b9d4b79aa34155a739fbd2c4c9d32db8218642101909b` |
| 新 RSA v2 签名 app | `0x121000` B；SHA-256 `69d73b5669462ea0e20b034098cd22efa470ee393e39d9dbd557045eaea18807` | `0x121000` B；SHA-256 `a042b2eced20a55200f762959a4c72ca25911578c315ddc31c3bc5185eba9001` |
| 新 ELF SHA-256 | `8e79ce8f3789ec51b16153ebd4407b7285d532be3a47852828f514ee297774ce` | `1738e3d5f68ec531abca3463dfbb6c76f274a6debf3a4608e1cde74f3da5feee` |
| `dependencies.lock` SHA-256 | `63259c2444b89238187a778563a6b52f873a5448e32eb1ac5b8d10b4b3763948` | 同左 |
| QEMU 完整日志 SHA-256 | `fbfbd90c7b8e23e4dd33219e6689f4397a227567af03e08b787f76318ed23e47` | `ff5c434cd488a90779bc1aa7266f2f49b632df197f92f1f3118ef3b69eb0f567` |

固定 SDK `python -m espsecure verify-signature --version 2 --keyfile <仓外测试键> <新签名 app>` 两侧均报告 RSA signature block 0 有效。以下命令只在具有上述冻结源目录的 mac-work-1 上复制、构建和 QEMU 运行，**不执行 `flash`**。逐套顺序构建；ESP-IDF Component Manager 共享索引并发配置会争用 `index.lock`。

```bash
source /Users/darrenyou/.cache/darren-space/esp-idf-578cf89/export.sh
python3 docs/operations/prepare_c3_frp_client_qemu.py iram-on \
  /private/tmp/esp32c3-frp-lazy-exact-20260927/probe \
  /private/tmp/esp-c3-client-qemu-20260927/iram-on
python3 docs/operations/prepare_c3_frp_client_qemu.py iram-off \
  /private/tmp/esp32c3-wifi-iram-off-20260927/probe \
  /private/tmp/esp-c3-client-qemu-20260927/iram-off
idf.py -C /private/tmp/esp-c3-client-qemu-20260927/iram-on/firmware \
  -D ESP_BASE_CONTAINER_BINDING_PROBE=ON build
idf.py -C /private/tmp/esp-c3-client-qemu-20260927/iram-off/firmware \
  -D ESP_BASE_CONTAINER_BINDING_PROBE=ON build
python3 docs/operations/frp_chunked_qemu.py \
  /private/tmp/esp-c3-client-qemu-20260927/iram-on/firmware \
  /private/tmp/esp-c3-client-qemu-20260927/iram-on/qemu.log 45
python3 docs/operations/frp_chunked_qemu.py \
  /private/tmp/esp-c3-client-qemu-20260927/iram-off/firmware \
  /private/tmp/esp-c3-client-qemu-20260927/iram-off/qemu.log 45
```

准备脚本要求目标目录尚不存在。签名时间与 `CONFIG_APP_COMPILE_TIME_DATE` 可能使将来重建的镜像摘要变化；复核应以输入锁、仅六行配置差异、签名有效、调用阶段和动态读数为主。QEMU 日志的 SHA-256 是上述本次实验的精确收据。
