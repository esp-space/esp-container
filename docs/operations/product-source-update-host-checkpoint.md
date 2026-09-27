# P6-11 同固件业务源码更新：主机检查点

2026-09-27，源码基线为 `esp-container@aaeec4b` 加本提交。沿用固定 wasi-sdk 33、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、现有 guest ABI 2、产品 ID `counter` 与 RSA-3072/PSS 临时测试签名链。没有更换固件、Container、WAMR 或包格式合同。

`examples/counter/counter.c` 的 v1 累计事件长度；新增 `examples/counter-v2/counter.c` 累计事件字节值。两份源码均经同一个固定编译配方、静态 ABI/profile 检查；本轮 v1 Wasm 为 469 B、SHA-256 `b9422cb4cb72983141988c4a9a59b602026d94729e2362403a04d1723f98a739`，v2 为 506 B、SHA-256 `f3ec18aba064a33d9ed51df00a178bdce29d398c1487c66e051e7aa0d4614849`。测试临时密钥、包与合成 Flash 不入库。

`slot_runtime` 主机集成在同一个 fixture boot 中让固件 A 保留 P0，固件 B 先确认 v1 的 P1，再在相同固件 SHA 下准备、试运行并确认 v2 的 P2；真实 WAMR 对同一输入 `{1,2,3}` 分别返回 **3** 与 **6**。随后 P3 保留 v2 行为，确认包在坏候选期间仍可回读验签。该路径执行包槽持久序号、签名/产品授权、临时映射、明确 init/event/stop/close；不是只改 manifest 版本号。

使用仓外临时构建目录与已有隔离 Python 环境执行：Python unittest **21/21**；锁定 WAMR、wasi-sdk 的主机 CTest **9/9**，其中 `slot_runtime` 通过。首次 CTest 因新增 v2 fixture 被误传给旧 `runtime_instance` 参数列表而失败 **8/9**；已将 v2 仅加入构建依赖，保持旧测试入参，重新配置、构建后 **9/9**。本轮未刷板、未修改 Base 依赖锁、分区或生产密钥。

此证据仅完成 P6-11 的公开源码与主机签名换包软件切片。Base 尚无设备 `product.*` 安装/结果入口，C3 和 ESP32 尚无冻结包分区及实板换包，固件 SHA/boot ID 的设备读回、网络峰值和恢复仍待逐板验证，P6-11 不写验收 ✅。
