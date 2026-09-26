# C3 OpenETH 与官方 FRPS 会话容量探针

## 范围与结论

本实验从[空闲 FRP client QEMU 容量探针](c3-frp-client-qemu-capacity-probe.md)使用的 **Wi-Fi IRAM 两项均关闭**的冻结输入，派生一份仅供 ESP32-C3 QEMU 运行的签名镜像。在 Base 报告 READY、ABI 2 标准 64 KiB guest 持续存活时，应用显式初始化固定 SDK 的 OpenETH，获得 QEMU 用户态网络 DHCP 地址，再用真实 `esp-frp` 公共 API `efrp_create → efrp_start → efrp_get_status → efrp_destroy` 连接宿主本地的官方 FRPS v0.71.0 `server.NewService`。服务端只监听 `127.0.0.1:29173`；guest 经 QEMU `open_eth` 用户态网络访问宿主映射 `10.0.2.2:29173`。证书、CA、Token 和固定可信时钟均只为仓外仿真创建，未消费生产凭据。

**确定的容量停止点：TLS 已 OPEN，尚未开始 FRP Login。** Client 从 `CONNECTING=1` 到 `TLS_HANDSHAKING=2`，随后以 `failure_phase=2`、`EFRP_NO_MEMORY=-20` 进入 `FAILED=8`；失败分配恰为 **5,552 B**，申请能力掩码 `6144`（`MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT`）。精确锁 `esp-frp@1f0c8f37db3765a74b3b95871bb266d0c73d1248` 的 `client.c` 只在 `efrp_tls_step` 成功后调用 `efrp_session_create`；后者先要求 `EFRP_TLS_OPEN`，再依次分配 session、4,096 B 登录缓冲和 `sizeof(efrp_yamux_t)=5,552` B。日志同时记录 `tls_error=0`、`verify=0`，与该路径一致。`phase=2` 是 client 在创建 session 成功前尚未切换状态，不应误写为 TLS 握手本身失败。没有 `AUTHENTICATING`、登录响应、代理注册、Pong 或会话内 64 KiB 认证记录的成功证据；按本次有界实验要求，在这个容量失败点停止。早先[手工满长 reader](c3-frp-client-qemu-capacity-probe.md)的 tag 认证属于**无 FRP session** 的另一阶段，不能代替本次未到达的会话内满长记录。

精确五仓输入为 Base `058e965671fa0e4d417114897541710699571c52`、FRP `1f0c8f37db3765a74b3b95871bb266d0c73d1248`、MQTT `9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`、OTA `207273188b984161362824c3344614e812016836`、Container `8eb805f3f12cb3cd836e9833acb4aca878ae80e7`。SDK 为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5`。`dependencies.lock` 记录本地 FRP 组件路径，需连同冻结输入和对应提交核对，不能单凭 lock 文件宣称源码版本。

## 动态读数

`free / largest / min` 均为 `MALLOC_CAP_8BIT` 字节；`min` 是开机以来的低水位，不随释放回升。阶段来自同一 QEMU 运行；下表不是生产板、真实 Wi-Fi 或 MQTT/OTA 并发读数。

| 阶段 | free / largest / min | 结果或失败 |
| --- | ---: | --- |
| Base READY 后，原有 4 KiB、坏 tag、无 client 满长认证记录 | 84,844 / 45,056 / — | 4 KiB 两次认证；坏 tag `-11` 且无明文；65,536 B 满长记录 `feed=0`、tag 认证、逐字节比较 65,536 B、16 块申请与释放 |
| 空闲 client 存活时的原有手工满长 reader | 74,920 / 45,056 / — | 仍是无 session 手工探针；`feed=0`、tag 认证、逐字节比较 65,536 B；reader 持有期间 9,320 / 3,584 B，清理后返回 74,920 / 45,056 B |
| OpenETH 前 | 84,844 / 45,056 / 6,164 | guest 持续存活；失败申请计数 0 |
| MAC/PHY 创建 | 72,340 / 45,056 / 6,164 | 成功 |
| Ethernet driver 安装 | 72,212 / 45,056 / 6,164 | 成功 |
| netif attach / IP handler | 71,448 → 71,416 / 45,056 / 6,164 | 成功 |
| `esp_eth_start` | 71,116 / 45,056 / 6,164 | 成功 |
| DHCP 回调 | 71,156 / 45,056 / 6,164 | `got_ip=1`；失败申请计数 0 |
| `efrp_create` 前 | 70,980 / 45,056 / 6,164 | 测试时钟和 CA 已设置 |
| `efrp_create` 后 / `efrp_start` 入队 | 60,532 / 45,056 / 6,164 | API 均返回 0；client 起初 `STOPPED=0` |
| `CONNECTING=1` | 59,944 / 45,056 / 6,164 | attempts=1 |
| `TLS_HANDSHAKING=2` | 31,960 / 18,432 / 6,164 | 尚未观察到 session 阶段 |
| `FAILED=8` | 60,212 / 45,056 / **4,620** | `failure_phase=2`、`error=-20`；失败申请计数 1、末次 5,552 B、caps 6144；`ready=0`、`pongs=0`、`tls_error=0`、`verify=0` |
| `efrp_destroy` 后 | 70,788 / 45,056 / 4,620 | API 返回 0，client 指针清空 |
| OpenETH 清理后 | 84,432 / 45,056 / 4,620 | 与 OpenETH 前相比 free 少 412 B；归属尚未证实，不能据一次运行判定泄漏 |

`probe_summary runs=2 failures=1` 中的 1 是预期的真实会话容量失败；第二次 guest `open/init/event/stop/close` 均为 0、事件结果 `guest=3`。仿真在 90 秒后由宿主 SIGTERM 结束，日志无 panic；Base 周期报告持续到约 85 秒。主机 FRPS 日志记录 `QEMU_FRPS_READY port=29173`；实验后向唯一测试 PID 发送 SIGTERM，记录 `QEMU_FRPS_STOPPED`，无遗留该测试监听进程。服务端日志设为 error 级，**不**提供逐条 TLS/FRP 协议 trace；TLS OPEN 的阶段定位依赖上述精确客户端源码、失败尺寸与状态组合，不应表述成服务端已独立确认登录。

## 派生、复现与边界

[OpenETH 准备脚本](prepare_c3_openeth_qemu.py)调用原有[client 准备脚本](prepare_c3_frp_client_qemu.py)，先校验冻结 IRAM-off 输入签名 app `4f1935a3898172cb3983600572e6808f11f9a3426783bd393ceeec45a258f059`、`sdkconfig` `230e7a60b88a59e98b4b9d4b79aa34155a739fbd2c4c9d32db8218642101909b` 和原始运行探针摘要，再加入[OpenETH 生命周期采样](openeth_qemu_probe.c)。[FRPS 准备脚本](prepare_c3_frps_session_qemu.py)加入[真实 client 探针](frps_session_qemu_probe.c)，生成临时 P-256 测试 CA 与带 `10.0.2.2` IP SAN 的证书，在 guest 中把系统时间固定为 2026-09-27 12:00 UTC，并嵌入测试 CA。`sdkconfig` 只由输入的 `# CONFIG_ETH_USE_OPENETH is not set` 改为 `CONFIG_ETH_USE_OPENETH=y`；IDF 构建另补入默认的 RX/TX DMA buffer 两项。原 Wi-Fi IRAM 两项继续关闭，没有修改产品默认。

[先前 ESP32 QEMU 报告](esp32-authenticated-qemu-capacity-probe.md)已经记录固定 SDK 的 `idf.py qemu` 会附带 `-nic user,model=open_eth`，但当时应用没有初始化该模拟网卡。本次按[Espressif 官方 QEMU Ethernet 说明](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32/README.md#ethernet-support)启用 `CONFIG_ETH_USE_OPENETH` 并实际运行 `esp_eth_mac_new_openeth`、`esp_eth_start` 与 DHCP，因此网络阶段有独立动态证据。

[宿主 FRPS wrapper](qemu_frps_fixture.go)复用 `esp-frp/tests/crypto-interop` 的官方 `github.com/fatedier/frp v0.71.0` Go 依赖和 `server.NewService`，配置严格 TLS 与 Token 心跳，且仅绑定回环；没有手写 FRP 协议替身。此 wrapper 是宿主测试工具，不属于五仓固件锁。先从本 Container worktree 向 mac-work-1 的仓外暂存目录复制下列脚本，再在持有冻结源工程和固定 SDK 的 mac-work-1 上执行第二组命令。准备脚本要求目标目录不存在；`29173` 必须先确认为空闲本地端口。Go fixture 在 QEMU 前启动，并在运行结束后按 PID 停止；测试服务绝不作为后台常驻服务安装。

```bash
ssh mac-work-1 'mkdir -p /private/tmp/esp-c3-frps-session-qemu-20260927'
scp docs/operations/{prepare_c3_frp_client_qemu.py,frp_client_qemu_probe.c,frp_chunked_qemu.py,prepare_c3_openeth_qemu.py,openeth_qemu_probe.c,prepare_c3_frps_session_qemu.py,frps_session_qemu_probe.c,qemu_frps_fixture.go} \
  mac-work-1:/private/tmp/esp-c3-frps-session-qemu-20260927/
```

```bash
export IDF_PATH=/Users/darrenyou/.cache/darren-space/esp-idf-578cf89
source "$IDF_PATH/export.sh"
python3 /private/tmp/esp-c3-frps-session-qemu-20260927/prepare_c3_frps_session_qemu.py \
  /private/tmp/esp32c3-wifi-iram-off-20260927/probe \
  /private/tmp/esp-c3-frps-session-qemu-20260927/probe --port 29173
idf.py -C /private/tmp/esp-c3-frps-session-qemu-20260927/probe/firmware \
  -D ESP_BASE_CONTAINER_BINDING_PROBE=ON build
python -m espsecure verify-signature --version 2 \
  --keyfile /private/tmp/esp-c3-frps-session-qemu-20260927/probe/test-key.pem \
  /private/tmp/esp-c3-frps-session-qemu-20260927/probe/firmware/build/esp_base.bin
cd /Users/darrenyou/darren-space/tooling/esp-frp/tests/crypto-interop
go build -mod=readonly \
  -o /private/tmp/esp-c3-frps-session-qemu-20260927/probe/fixture/frps-server \
  /private/tmp/esp-c3-frps-session-qemu-20260927/probe/fixture/qemu_frps_fixture.go
/private/tmp/esp-c3-frps-session-qemu-20260927/probe/fixture/frps-server \
  -port 29173 \
  -cert /private/tmp/esp-c3-frps-session-qemu-20260927/probe/fixture/frps_server.pem \
  -key /private/tmp/esp-c3-frps-session-qemu-20260927/probe/fixture/frps_server_key.pem \
  > /private/tmp/esp-c3-frps-session-qemu-20260927/frps.log 2>&1 &
FRPS_TEST_PID=$!
trap 'kill -TERM "$FRPS_TEST_PID" 2>/dev/null || true' EXIT
python3 /private/tmp/esp-c3-frps-session-qemu-20260927/frp_chunked_qemu.py \
  /private/tmp/esp-c3-frps-session-qemu-20260927/probe/firmware \
  /private/tmp/esp-c3-frps-session-qemu-20260927/qemu-frps.log 90
kill -TERM "$FRPS_TEST_PID"
wait "$FRPS_TEST_PID" || true
trap - EXIT
```

实际启动命令含 `qemu-system-riscv32 -M esp32c3 ... -nic user,model=open_eth -no-reboot`。签名 app 为 `0x131000` B，小于现有 `0x1e0000` B 最小 app 槽；`espsecure` 验证 RSA v2 signature block 0 有效。证书与私钥每次随机生成，未来重建的签名 app 摘要也可能因构建时间和证书不同，故以下 SHA-256 只是本轮精确收据：

最终准备脚本另从同一冻结输入重放到新临时目录；生成的 `capacity_runtime_probe.c`、空闲 client C、OpenETH C、FRPS C 与 `CMakeLists.txt` 逐文件摘要均与实际构建工程一致。重放只检验派生内容，未代替上文实际构建和 QEMU 运行；重放临时目录已删除。

| 制品 | SHA-256 |
| --- | --- |
| 构建后 `sdkconfig` | `a66858cf817841457a8557a46ec18e5757e4889a5e31dffc0978adf8a277fb52` |
| `dependencies.lock` | `63259c2444b89238187a778563a6b52f873a5448e32eb1ac5b8d10b4b3763948` |
| 签名 app | `718d29e9e24ce493797106b924f0a1c39eb2e92a3afe4539d983ddc140cfae75` |
| ELF | `1fba1a57c269f3f2d63f93cd52f3ea40c3255f4e82efb087362ac143affea5e6` |
| 完整 QEMU 日志 | `59dd679bb23a6d70291f635fae4bec45a5ffb2ee0f0ad0d6da215874935d502b` |
| 测试 FRPS 停止后的日志 | `650e345436ea149e58a5ba7bcfd579c5ff2aaf5d8f4e5be26a5d79be37df8e75` |

镜像仍包含公开测试签名键、UART0 QEMU 控制台和 ADC2 校准空桩，**不可刷物理设备**。没有修改生产 CA/Token、产品默认配置、实际网络服务或设备 Flash/eFuse。此实验也没有证明真实 Wi-Fi、MQTT Broker、OTA 下载与 FRP/guest 的并发容量，更没有完成 P6-03 验收。下一次要走到登录/注册/Pong和会话内 64 KiB 认证记录，须先在保留标准 guest、严格 TLS 与完整记录的前提下解决本次 5,552 B 分配失败，并重新按阶段测量；不能把这个前置失败后未运行的阶段记为通过。
