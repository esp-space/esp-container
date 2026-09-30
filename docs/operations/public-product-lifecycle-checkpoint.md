# 公开产品生命周期接线检查点

## 接口与边界

`components/esp_container/include/esp_container_product.h` 向 Base 产品装配公开受限的槽选择与 `open/init/on_event/stop/close`，以及现有日志、定时器导入的 owner 驱动读取/投递。组件没有公开 `econtainer_runtime_open` 裸 Wasm 入口、任意包字节入口或第二份活动包状态。`product_open` 直接复用既有私有槽装载：在共享槽锁内读取单 blob、核对实际可启动固件集合和预期序号，只选择运行固件的确认绑定或同一 boot 且处于 `TRIAL_STARTED` 的精确 operation；随后从选中槽映射完整包，对映射中的字节重新验签、验摘要、核对产品/ABI/schema/独立授权和限额，再打开 WAMR。映射在解锁及返回实例前解除。无包绑定返回 `EMPTY`，不会虚构 guest。

调用方必须提供如下真实输入：

- 由 Base 在 app/otadata 串行 owner 下核对的已签名可启动固件 SHA-256 集合；该集合从调用开始到结束不得变化。Container 只核对传入值与持久绑定，不证明底层 app 身份。
- 经冻结分区表验证的三槽 `io/geometry`，包括精确专用包区、独立 NVS 单 blob 及覆盖所有包写入者的锁。这个槽锁与 Base 外层 owner 不得是同一个非递归锁。
- 持久记录的 `expected_sequence`；试运行还需当前 boot 的原始 `operation_id` 与 `boot_id`，并先完成 `TRIAL_STARTED` 持久提交及读回。确认选择必须把两个 ID 字节数组置零。
- 独立可信的产品 ID、RSA 公钥及 key ID、设备能力授权、包资源限额、同步验证工作区；签名清单不能自行授予能力。运行限额与签名值取更严格边界。若允许 LOG/TIMER 导入，同一个执行 owner 还须读取日志与轮询期限，不由本组件从后台进入 guest。

同一实例只由唯一串行 owner 调用。ESP-IDF 的执行 owner 必须是 `pthread_create` 线程；原始 FreeRTOS `xTaskCreate` 任务没有 WAMR 所需的 pthread 身份。`open` 成功后才调用 `init`；失败、trap 或停止失败均不得再投递事件。调用方先证明自身原生引用已回收，再调用 `close` 释放组件资源；`close` 的成功不能冒充业务健康或持久 `CONFIRMED`。本 API 不替 Base 管理 MQTT/FRP 回调队列、网络期限、外部动作、升级授权或持久操作回执。

2026-09-30 装载结果补齐本次重新验签的产品 ID、完整版本切片和 guest ABI／data schema。切片仅引用调用方 `package_workspace->manifest`，解除 Flash 映射后仍有效，直到调用方修改工作区；跨工作区复用或需要保留到实例运行期时由调用方先复制。六字段只在两结果均 OK 且实例非空时填充，所有失败保持零；没有增加验包遍历、组件 heap 缓存或版本字符串新上限。签名清单的 4096 字节总上限和精确确认／trial 选择保持，结果只证明本次选中实例的装载来源，不证明 init、健康或持久操作成功。

## 软件验证与尚缺

`slot_runtime` CTest 使用公开头和公开入口运行真实 RSA-3072 测试签名包与锁定 WAMR，覆盖双固件 P0→P3、确认/试运行、错误固件/operation/boot/序号、错误签名映射、独立授权、引擎失败、单实例 BUSY、并发写锁、map/unmap 配对、宿主日志和定时器投递。测试密钥只存在临时目录。CMake 的公开头消费者不再取得组件私有 include 目录。测试结果以本次执行收据为准；此处不把 host 通过扩展为实板结论。

2026-09-27 在隔离副本执行：锁定 WAMR `26c235e53e29acd8b43abe7f3b524577bd4d1ae5` 与校验过 SHA-256 的官方 wasi-sdk 33 下，主机 CTest 9/9、Python unittest 20/20 通过。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 上，C3 与 ESP32 独立样例均完成构建，两个组件归档均编译 `product.c`；镜像 SHA-256 分别为 `c154de762f87123c636942e7c0dc0e14783be3baea802385e91da2df612c5a4b`、`53e3d76561aebf3243d129258f61af2a2787875fb4b71ed7b1566bb5a19a4bf8`。独立样例没有调用公开产品 API，也不含 Base/FRP/MQTT/OTA 产品组合，故它们的构建不能证明最终产品 ELF 保留该链路。

2026-09-30 当前消费边界：Base 候选已具备双目标产品分区源码、同一 claim 下的确认／pending 签名固件观察、公开产品命令及联合 OTA 软件链；实体板仍是旧布局，物理迁移、真实网络与资源、掉电和正式发布未验收。本次新增装载元数据尚未接至 Base 设备回读和 Tool 活动版本／trial 展示，不将组件 API 或独立样例编译计为完整 P8-04。P6-03、P7-01/P7-02 与双板验收继续开放。
