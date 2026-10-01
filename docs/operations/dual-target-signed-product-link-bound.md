# 双目标签名 Base：Container/WAMR 未链接的产品尺寸下界

## 裁决

同一组精确五仓源码在固定 ESP-IDF/lwIP 上，以 **Base 原样主应用**、临时测试签名键和 `ESP_BASE_CONTAINER_BINDING_PROBE=ON` 分别完成 ESP32-C3 RSA v2 与 ESP32 ECDSA v1 构建。两份签名镜像均通过官方验签和现行双 app 槽尺寸检查。C3 为 **1,052,672 B（`0x101000`）**，ESP32 为 **983,028 B（`0xefff4`）**；与各自[当前 Base 签名单体](https://github.com/esp-space/esp-base/blob/7ec2738d678735a7f8e2965325ceac2d3f98c9f8/docs/operations/development-checkpoint.md)的文件长度相同。

关键限制是 **Container、binding、WAMR 均未链接进产品镜像**：三个静态库确实编译生成，但两目标 map 均只有对应 `LOAD` 行，没有任何 `lib*.a(成员)`；最终 ELF 也没有 `esp_base_container_reconcile`、`econtainer_runtime_open`、`econtainer_slots_reconcile` 或 `wasm_interp_call_wasm`。正式 Container 实例与业务包安装入口尚未接入 Base 主应用，主应用没有 Container 安装、对账或运行调用。FRP、MQTT、OTA 则分别有 13、5、4 个库成员出现在 map，并保留 `efrp_tls_step`、`emqtt_create`、`esp_mqtt_client_start`、`eota_preflight`。所以这两份镜像只给出**无 fixture 的 Base 实际链接下界**，不能称为五能力组合固件，也不证明 Container 闪存或 RAM 可容纳。P6-03 与 P7 仍未验收。

## 输入与操作边界

| 输入 | 精确值 |
| --- | --- |
| Base / FRP / MQTT / OTA / Container 组件 | `7ec2738d678735a7f8e2965325ceac2d3f98c9f8` / `1f0c8f37db3765a74b3b95871bb266d0c73d1248` / `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2` / `207273188b984161362824c3344614e812016836` / `8eb805f3f12cb3cd836e9833acb4aca878ae80e7` |
| SDK / 实际 lwIP / WAMR | `578cf89c343e388db43ba1f4ddcd602fedcb763c` / `2758df4cd3666b3b2a5b53830148379326425c0d` / `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`；WAMR 受锁缓存 `.component_hash` 为 `a799be27248cadffdee6f6fae988dbdf3f5d423baab0b1008d74b8581b5ed507` |
| 仓外测试 | mac-work-1 的固定 SDK；新建 RSA-3072、P-256 测试键仅留在 `/private/tmp/esp-dual-signed-product-link-private-20260927/`，未复制进 Git、镜像 receipt 或产品 defaults |
| 主应用 | Base Git 归档原样 `firmware/apps/esp_base/main`；无 `capacity_references.c`、`capacity_runtime_probe.c`、`runtime_guest_bytes.h`，也没有密文、guest 或虚构业务调用 |

[准备脚本](prepare_dual_target_signed_product_link.py)先对五个完整提交执行 `git archive`，记录归档 SHA-256；装配阶段再次核对五归档和受锁 cJSON/WAMR 缓存摘要，仅把四个精确组件源码复制到各自仓外 Base 固件工程、追加 Container 的 target Kconfig defaults，并在工程外写入签名配置。Base 主应用、组件实现和产品分区表均未改动。[机器收据](dual-target-signed-product-link-receipt.json)保存上述 Git 归档 SHA、实际 SDK/lwIP、生成配置/依赖锁/分区表及 signed bin/ELF/map 摘要、签名检查结果、保留符号与各库成员。依赖锁中四个组件是仓外本地路径，来源由归档完整提交和摘要约束；不能把路径版本 `0.1.0` 当作源码版本。

没有连接设备、Broker、FRPS 或 OTA 服务，也没有写 Flash、eFuse、NVS 或生产签名键。C3 原板旧 `base_store` 异常页和 ESP32 旧 AT 首次迁移仍由各自只读证据及恢复合同裁决；本次编译不能授权刷写。

## 两目标结果

| 结果 | ESP32-C3 RSA v2 | ESP32 ECDSA v1 |
| --- | ---: | ---: |
| signed `esp_base.bin` 精确文件长度 | **1,052,672 B，`0x101000`** | **983,028 B，`0xefff4`** |
| signed SHA-256 | `4901cae7c7f2b1d91926e0e142a0a6e2e42e0d31b0bd18ee667e16fa07f78b7a` | `0838a19d1ddca914de2c7c04e22b86c238342e31ded257357dfd61fc782d472a` |
| 固定 SDK 官方验签 | `Signature block 0 verification successful` | `Verifying 982960 bytes of data... Signature is valid.`；982,960 B 是验签数据长，不是文件长度 |
| 现行最小 app 槽／官方剩余 | `0x1e0000`／**913,408 B（`0xdf000`）** | `0x120000`／**196,620 B（`0x3000c`）** |
| bootloader／到分区表剩余 | `0x53e0`／`0x2c20` | `0x5b10`／`0x14f0` |
| FRP / MQTT / OTA map 成员数 | 13 / 5 / 4 | 13 / 5 / 4 |
| Container / binding / WAMR map 成员数 | **0 / 0 / 0** | **0 / 0 / 0** |

三个 Container 相关库的 `LOAD` 行和 `.a` 实体只说明 CMake 编译、链接器看到了库；map 没有取出任何成员且 ELF 没有上述 Container/WAMR 入口，构建没有保留包验签、三槽、guest runtime 的产品字节。两目标 `efrp_tls_step` 等符号存在也仅说明相应 Base 调用路径进入镜像，不能推定 TLS/FRPS、MQTT Broker、OTA 实际运行时的堆峰值。

对照不可混淆的三个量：

| 目标 | 当前 Base 单体签名镜像 | 本次无 fixture + 可选 binding 编译 | 既有强制链接/QEMU fixture 镜像 |
| --- | ---: | ---: | ---: |
| C3 | `0x101000`，SHA `3aecff80…d1167` | **`0x101000`** | `0x121000`，强制保留 Container/WAMR 并嵌入 guest/认证 wire |
| ESP32 | `0xefff4`，SHA `1bd24b89…d1390` | **`0xefff4`** | `0x10fff4`，强制保留 Container/WAMR 并嵌入 guest/认证 wire |

本次大小与 Base 单体恰好相同，不代表二进制逐字节相同；测试键、编译时间与额外 Kconfig 不同。既有 fixture 镜像分别比本次大 131,072 B，但同时改变了强制链接、探针与嵌入数据，不能把差额全部归因于 Container 实际产品装配。QEMU 的 heap 与 AEAD 记录读数只适用于其各自的 fixture，不能套在本次无探针镜像上。

## 4 MiB 候选几何的实际含义

C3 **现行**两个 `0x1e0000` app 槽都能容纳本次签名 Base，却没有独立三包槽。已有[仓外三包槽候选表](five-component-kconfig-ab-partition.csv)将两个 app 槽各设为 `0x118000`，`product_pkgs` 为 `0x186000`；固定 SDK 的 `gen_esp32part.py --flash-size 4MB --secure v2` 与 `check_sizes.py --type app` 对本次 C3 signed bin 均通过，各 app 槽仅余 **94,208 B（`0x17000`，8%）**。这只是**Container/WAMR 未链接镜像**的 app 余量。既有 C3 强制链接镜像 `0x121000` 比该槽大 `0x9000`，而真正产品 Container 入口尚未装配；不能用本次较小镜像宣称该候选布局成立。候选保持旧 `base_store@0x3e0000/0x20000` 的原始地址并改标 `base_archive`，另在 `0x138000` 新建活动 `base_store`；旧区原始字节保全和新 NVS 转换未经实板验证，不是已完成迁移。

ESP32 当前离线表已有两个 `0x120000` app 槽和 `product_pkgs@0x260000/0x186000`；官方 `--secure v1` 解析通过，本次 signed Base 每槽余 196,620 B。包区域的几何存在不代表 Base 已调用 Container、完成真实三槽安装或旧 AT 归档。两个目标都还需在真实产品调用链中链接包验证、固件集合绑定、WAMR 和实际包，并复测 signed 文件、RAM 峰值与同板并发，再决定分区。不能以剔除 TLS、签名、回滚或身份来凑容量。

## 复现

在能读取五个精确提交的 checkout 上用[准备脚本](prepare_dual_target_signed_product_link.py)的 `export --base … --frp … --mqtt … --ota … --container … --output <bundle>` 生成仓外源码包；将包和脚本送到具备固定 SDK 的宿主。新建仓外测试 RSA-3072 与 P-256 键，然后以 `assemble --bundle <bundle> --output <probe> --managed-cache <固定 cJSON/WAMR 缓存> --c3-key <仓外 RSA 键> --esp32-key <仓外 P-256 键>` 准备两目标独立工程。目标路径须不存在；脚本拒绝覆盖。实际 mac-work-1 输入包位于 `/private/tmp/esp-dual-signed-product-link-inputs-20260927/`，工程位于 `/private/tmp/esp-dual-signed-product-link-20260927/`，五归档摘要在收据中。

分别在 `source "$IDF_PATH/export.sh"` 后构建；C3 的 `SDKCONFIG_DEFAULTS` 顺序为 Base 共用、`sdkconfig.defaults.esp32c3`、Container C3、仓外 `signed.defaults`，ESP32 换成对应的 target defaults。两者都传 `-DESP_BASE_CONTAINER_BINDING_PROBE=ON`，使用独立 `-B` 与 `-DSDKCONFIG`；ESP32 另传 `-DIDF_TARGET=esp32`，**不传**无签名离线开关。签名配置选择 C3 RSA v2 更新验签、ESP32 ECDSA v1 启动与更新验签，并保留两者的 rollback。通过固定 SDK 的 `espsecure verify-signature` 分别使用 `--version 2` / `--version 1` 校验完整 `.bin`；用目标工具链 `nm --defined-only` 导出 ELF 符号，再以[审计脚本](audit_dual_target_signed_product_link.py)的 `--root <probe> --bundle <bundle> --idf "$IDF_PATH"` 从实际 map、日志和文件输出收据。该脚本若发现主应用混入容量 fixture、构建/验签记录缺失或归档摘要漂移会失败。

所有命令限于仓外 `build`、签名和只读核对；不要将 `idf.py` 提示的 `flash` 命令用于旧板。本次没有生成正式 release、安装业务包或 OTA rollout。
