# 三包槽存储软件检查点

本检查点实现 `esp_container_slots.h` 的受控原始 Flash 包槽、单 blob 记录与双固件集合对账，并加入联合固件/包切换的软件状态合同。它服务于跨仓计划 P6-08/P6-10 的软件切片，**尚不是可发布的包安装或双固件联合升级**。没有写入设备或修改真实分区表。

## 同产品同版本制品身份

计划 9.3 的身份门从当前操作实际保留的两份固件绑定取引用，不保留历史版本库。`write_and_prepare` 包括普通安装／升级和联合固件 WRITE，`stage_firmware` 的 REUSE，以及公开 `product_open` 均向验包回调借用同锁引用视图和包内读取器；视图不能保留或重入槽操作。REUSE 先退役旧 inactive 固件，比较 `next` 中仍保留的引用，旧包的冲突或损坏不再阻止合法切换。WRITE 已持久化的 WRITING 视图本身就是准备后的集合。

异摘要引用用现有工作区完整验签、核完整整包摘要和自身持久 ABI/schema，再比较已验签产品 ID 与完整版本字节；不把当前固件的 grant 或 ABI 强加给合法回退包。相同产品和相同完整版本但不同整包 SHA 返回 `UNTRUSTED`，随机 PSS 重签同样拒绝；不同版本或另一产品不构成此冲突。同 SHA 引用先要求大小／ABI/schema 一致，再复用候选的完整验签和槽引擎同锁全摘要回读证明，不新增一次完整引用验签。失败保持原相位及清退合同，不返回部分签名元数据。

没有新 heap、全局缓存、第二份 manifest 或持久字段。异 SHA 引用最多两份，每份只读块不超过 512 字节；验包与比较顺序复用已有 4992 字节工作区。额外逻辑读量由引用完整验包的实际读取量加版本比较量组成，版本比较每份最多读取其完整版本且不超过既有 4096 字节清单上限。该界限不是 Flash 耗时或运行栈峰值保证；真实 SDK 成本、实板 I/O 和组合容量须分别验证。

## 事实与边界

- 调用方从 Base 的实际分区发现中提供独立 data 分区边界、三个互不重叠的槽、擦写粒度；本仓没有冻结物理 offset、槽长或 4 MiB 布局。几何不完整、未对齐、越界或重叠会在调用任何回调前拒绝。
- `read_blob` 必须精确区分 NVS key 不存在、完整读取和读取失败，且只能从已提交的持久视图读取，不得返回同一 NVS handle 的未提交缓存。`write_blob` 只有对**同一个 key**完成 `set_blob`、`commit` 才能返回 true；组件随后精确读回 288 字节。commit 返回 false 即使读到新字节仍视为结果不明，不据此擦槽。CRC32 检测意外损坏，不承担数据真实性。记录非法、读回未知或序号不符时不擦槽。
- `lock` 必须覆盖所有包槽 Flash/NVS 写入者和运行切换者，且在整个操作期间保持独占。组件不包含第二套操作账本；Base 对外仍负责跨多轮 operation_id 的去重、权限与串行化。单 blob 仅保留最近一次操作 ID 与相位。
- 迁移入口 `initialize` 仅接受缺失记录，并对现有包引用全量回读 SHA-256；两份绑定还必须与调用方报告的实际可启动固件摘要集合精确相等。调用方必须事先核对实际 app/otadata、包签名、授权、ABI 与产品归属。读取失败、CRC 错误和真实空记录严格不同，不能以初始化覆盖损坏记录。
- 任一擦除前重新读取记录、核对预期序号、全量计算两份已确认包摘要，并先把唯一候选槽写为 `WRITING`，完成 NVS commit/精确读回。一个包槽被任一已确认绑定或未决候选引用时不能被选为擦写目标。
- 当前固件已有业务包时，product-only 更新要求候选 `data_schema_version` 精确相同；尚无包时可首次安装。产品 ID、签名密钥、ABI 与所需能力仍由验包/授权回调精确核对。
- 写包时只擦已持久保留的槽，按提供的写粒度写入，不足末块补 `0xff`。写完从 Flash 重新计算**精确包长** SHA-256，再让调用方的只读回调完成签名、Wasm、产品授权与宿主 grant 检查；现有 `econtainer_package_slot_validate()` 可承担这段回读准入。回调区分校验期间的 Flash 读失败 `IO_FAILED` 和完整读取后的信任拒绝 `UNTRUSTED`。任何失败留在 `WRITING`，不能自动试运行；校验通过且 blob 读回一致后才进入 `PREPARED`。
- `begin_trial` 在旧实例已停止回收之后、启动新实例之前持久记录 boot ID；`mark_healthy` 仅接受同一 boot ID；`confirm` 再读回受保护引用，并在单 blob 内确认该固件的包绑定。Container 不读写 `otadata`，也不能仅凭当前运行摘要推断 OTA 固件已 VALID；联合切换由 Base 在 `esp-ota` 确认 VALID 后才调用 `confirm`。
- 启动对账 `reconcile` 先重新计算两份确认包摘要。product-only 未决操作继续返回旧确认绑定；联合固件操作在旧固件运行时也只允许恢复旧确认包，在新固件运行且状态为 `PREPARED` 时明确返回 `BOOT_START_TRIAL`，由 Base 生成本 boot ID 并先提交 `TRIAL_STARTED`。新固件运行但包仍在 `WRITING`、旧 trial 已跨 boot、已取消，或新固件确认后却运行旧固件，均返回 `CONFLICT/BOOT_BLOCKED`。候选损坏在旧固件运行时保留原始 `UNTRUSTED`/`IO_FAILED` 与 `BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID`；在新固件运行时保持 `BOOT_BLOCKED`。确认包错误也始终阻断。
- `abandon` 为显式取消。不同 boot 可直接证明 RAM 中不存在前次试运行实例；同一 boot 必须由唯一执行器 owner 在持有存储锁期间调用 `trial_stopped_fn`，核对该 operation 的候选已停止、回调/原生引用已收敛。取消只依赖确认包完整性，被抛弃的候选字节即使损坏也不妨碍持久取消。取消后的新操作仍须取得不同 operation ID，并重新执行授权/验包。调用方在启动旧包前仍须验证签名、ABI、授权、数据 schema 和运行能力。

## 当前固件包绑定清除

`econtainer_slots_uninstall` 是 product-only 的持久转换：Base 必须持有 app/package 串行所有权，先确认当前选中固件的 OTA 收据已经收敛，再停止并回收当前已确认实例，然后提供真实可启动固件集合、当前序号、全新 operation ID 和预期包 SHA-256。唯一执行器的 `instance_stopped_fn` 在包槽锁内核对被清除的精确绑定已停止且没有原生引用；回调不得重新进入槽 API。当前固件没有该包、固件集合或序号变化、未决 trial、Container 联合固件转换未收敛时均拒绝。Container 无法自行读取 Base 的 OTA 收据；此 API 不承担命令授权、实例调度和跨历史 operation ID 的账本。

转换在原 ECS2 blob 中只清当前运行固件的包字段，保留固件摘要、另一可启动固件的绑定、包 Flash 全部字节与产品数据。提交前重新计算转换后仍被引用的全部包 SHA-256；损坏的待卸载包可移除，但损坏的回退包不得被宣称可恢复。成功时 `CONFIRMED + NO_PACKAGE + firmware_transition=false` 保存本次 operation ID，且 trial boot ID 为空。Base 可在独立读回后把它作为卸载结果；当前固件重启对账取得无包确认绑定，回退固件仍能对账到原包。无引用的旧包槽只能在后续新操作先持久预留后被擦写，卸载本身不擦槽。

NVS 写前失败且读回仍是旧 blob 返回 `IO_FAILED`；commit 可能已生效但返回失败，或写后读回失败，返回 `UNCERTAIN`。Base 必须在重新装载持久记录后按序号、operation ID、固件摘要与包绑定判定结果，不能将不确定返回视为卸载成功或立即重试擦写。现有 `slots_test` 和真实 `slots_idf.c` provider 假 SDK 集成测试覆盖这两种提交边界、重新绑定 provider 后的状态读回及回退引用保护；它们不代表 Base 已具备命令入口或物理掉电证明。

截至本切片，Base 的 V2 OTA 收据若为 `SUCCEEDED` 且仍选中 C，每次启动仍要求 ECS2 保留那次联合 OTA 的原 operation ID、`firmware_transition` 和精确 sequence。清除绑定会替换这条最近操作记录，使下次启动阻断；后续 product-only 安装也有同样问题。因此 Base 在接入此 API 前必须把 `PREPARED` 收据的严格恢复与 `SUCCEEDED` 收据之后的正常 ECS2 对账区分，并验证后续包操作的重启状态。此处的 Container API 和主机测试仅证明局部存储转换，不构成 Base 可消费或设备可发布的卸载功能。

## 双固件集合对账软件切片

`initialize`、`reconcile` 和 `reserve` 接受同一 `econtainer_slot_firmware_set_t`：一或两份实际可启动固件 SHA-256，以及其中正在运行的一份。集合不区分数组顺序；零摘要、重复、越界计数、运行固件不在集合均拒绝。已提交 blob 中的固件绑定必须与集合完全一致；旧固件已被 OTA 覆盖却仍留在绑定中，或新固件出现却没有绑定，启动选择返回 `BOOT_BLOCKED`，擦槽预留返回 `CONFLICT`。这不会自动删除旧包或把新固件判作已确认。摘要校验继续覆盖两份绑定，因此从当前固件启动也不能忽略回退固件的包损坏。

这个参数必须由 Base 从实际 app 分区与 `otadata` 读取和校验；Base 还须把固件切换与包操作放在同一串行操作所有权下，确保集合快照在调用期间不变。Container 目前只核对调用方给出的值，无法自行证明物理可启动性、app 签名、OTA 状态或旧槽已经失去回退资格。物理分区几何没有因此确定，当前 Base 分区仍不能绑定包区。

## 联合固件与包切换的软件合同

`stage_firmware` 仅在 Base 已将**签名验证通过**的新镜像写入原备用 app 槽、原备用镜像确实失去可启动资格、尚未选中新镜像时调用。调用方给出实际旧/新两份摘要、当前运行旧摘要、预期序号和全新 operation ID。Container 在原 288 字节单 blob 中硬切为 `ECS2`，同时更新备用固件绑定、记录 `firmware_transition` 和包决策；它没有第二份账本，也不会自动读取旧 `ECS1`。当前真板没有 Container blob，既有 Base NVS 不会由此擦除或迁移。

- `PACKAGE_WRITE`：先在 blob 中持久预留未被旧运行固件绑定的槽，返回 `WRITING`；随后 `write_and_prepare` 才能擦槽、写包、全量回读摘要和用 `econtainer_package_slot_validate` 验签授权，成功后提交 `PREPARED`。被替换的旧备用固件及其包不再计入保护集，仍运行的旧固件包保持受保护。
- `PACKAGE_REUSE`：只可引用旧运行固件的已确认包；`econtainer_package_slot_validate_binding` 按新固件的独立产品/grant 输入重新验签、检查 Wasm 与配额，单次提交备用固件无包绑定及 `PREPARED`；直到最终确认才共享该包槽，不复制 Flash。
- `NO_PACKAGE`：明确记录新固件没有 guest，直接提交 `PREPARED`；试运行选择得到 `EMPTY`，健康与确认仍须由 Base 明确推进，不能把无包等同于固件健康。

Base 只可在 `PREPARED` 持久读回后调用 OTA 选择；新镜像实际启动并经 Base 签名/otadata 核对后，按 `begin_trial → 业务健康验证 → mark_healthy → esp-ota 标记 VALID 并回读 → confirm` 顺序推进。`confirm` 不提供 OTA 状态证明，必须由调用方持有同一串行 owner 保证顺序。若在 `HEALTH_VERIFIED` 后复位，`reconcile` 对运行中的目标固件仍返回 `CONFLICT/BOOT_BLOCKED`，不能重启 trial；只有 Base 独立读回目标签名固件为 OTA `VALID`、持久记录与真实固件/包相符并完成本次本地启动基本检查，才可使用记录中原 trial boot ID 补交 `confirm`，不能重放外部业务动作。若状态仍为 pending、记录不匹配或完整性检查失败，保持阻断。失败回到旧镜像时，Base 先显式 `abandon`，待实际 OTA 状态证明新目标不再可启动，才可用 `drop_aborted_firmware` 删除其绑定；这两个 API 都不擦包槽。读回不确定时不得选新镜像或擦槽，须重新读取唯一 blob 裁决。主机测试覆盖写包、复用真实签名包、无包、旧备用包损坏、错误授权、commit 不确定、旧 trial 重启阻断与失败回退；真实 OTA 调用和掉电验证仍未接入。

### 中断写入后的旧备用绑定退役

`econtainer_slots_retire_inactive_firmware` 服务于 OTA 下载已开始改写备用 app 槽、但尚未形成可启动候选的恢复路径。Base 必须在调用前独立核对持久 OTA 写入收据、正在运行的旧固件 A 的签名与 OTA `VALID`、原备用固件 B 的精确摘要，以及物理 app/otadata 中 B 已不可启动的证据；同时持有与 OTA、包槽共用的固件/存储 owner。Container 只收到 A-only 的实际可启动集合、被退役的 B 摘要和预期 blob 序号，不能自行验证这些外部事实。

该入口在槽锁内只接受 `IDLE` 或 `CONFIRMED`，核对 A/B 绑定、两份已确认包的完整 SHA-256 引用，然后把 B 绑定和已完成 operation 一起清为 `IDLE`，以同一个 ECS2 blob 提交并读回。它不擦除包槽，不自动继续下载，也不改变 OTA 状态；完成后现有 `stage_firmware` 可从 A-only 进入新的 A/C `PREPARED`。任何未决相位、错 B、错 A、错序号、损坏包/记录和未知持久写入结果均不得退役。对于已是 `IDLE` 的 A-only 状态，使用**当前**序号调用时会重新核对 A 包后无写入返回；这仅是安全重入，不证明历史 B 身份或外部 OTA 收据。旧序号仍返回冲突。commit 或读回结果不明时，Base 必须重新读取 blob 与物理证据后裁决，不能直接重试写 app。

主机假 Flash/NVS 测试覆盖 A/B→A、`CONFIRMED` 历史记录清除、A-only 重入、随后 A/C 准备、WRITING/PREPARED/TRIAL_STARTED/HEALTH_VERIFIED/ABORTED 拒绝、错误固件集合、两份包引用损坏、读故障、锁忙、commit 前/后故障、读回失败及撕裂 blob。独立工作树在 AppleClang 的严格 ASan/UBSan 下完成本机 CTest **6/6**。此软件入口本身尚未证明 Base 收据与 app/otadata 物理状态对账或掉电恢复。

2026-09-27 的独立工作树复核：锁定 WAMR 与 wasi-sdk 33 下主机 CTest **9/9**、Python unittest **运行 14 项，其中 1 项因工具链条件跳过，其余通过**；`slots`、`package_slot`、`slots_idf` 分别以严格 ASan/UBSan 运行通过。固定 IDF `578cf89` 的 C3 与 ESP32 独立样例编译通过，app 大小分别为 `0x37920`、`0x35000`，SHA-256 分别为 `0835366e7ecd1424c5b14772a0cf7e1f0edbe0206d0e3fac65e119aa5bd28c4f`、`ebee2fd8104ae9702f8ba2b3b8c46426df27a98dbeb4ce2dab0411e01b30e09c`。样例未装配 Base/FRP/MQTT/OTA，也未调用联合切换入口，故尺寸不代表产品组合余量。

## 三槽引用序列

| 时点 | 旧固件确认包 | 当前固件确认包 | 未决/新包 | 下一个可擦槽 |
| --- | --- | --- | --- | --- |
| 初始 | P0：槽 0 | P1：槽 1 | 无 | 槽 2 |
| P2 写入/试运行 | P0：槽 0 | P1：槽 1 | P2：槽 2 | 无 |
| P2 确认 | P0：槽 0 | P2：槽 2 | 无 | 槽 1 |
| P3 写入/试运行 | P0：槽 0 | P2：槽 2 | P3：槽 1 | 无 |
| P3 确认 | P0：槽 0 | P3：槽 1 | 无 | 槽 2 |

相同包摘要的共享槽可以留在两份固件绑定中；该槽的摘要、长度、ABI 与 schema 必须完全一致。本切片没有做共享包的下载去重。记录只保留两个固件摘要；新增/淘汰绑定只能由 Base 已验证的实际 OTA 状态触发，不由 Container 猜测镜像身份。

## 已验证与待验

主机 `slots` CTest 使用假 Flash/NVS 覆盖 P0→P3 引用变化、两份固件各自重启对账、错误/缺失/重复固件集合的启动与擦槽拒绝、超过实际单槽容量的候选拒绝、先提交保留再擦除、两种 commit 返回错误的保守裁决、blob 读回失败/CRC 损坏、部分写入、候选回读摘要损坏/断读、旧确认包可恢复但不可推进候选、同 boot 无停止证明拒绝取消/停止证明后取消、错序号/错固件与几何拒绝。原包解析和 Wasm 扫描 CTest 保持独立运行。

新增 `package_slot` 主机测试把真实签名包、三槽假 Flash 回读和独立授权组合，并拒绝错误产品、schema、key ID、内存/队列/指令/期限及读故障。联合状态合同增加复用时不同产品授权拒绝、按新固件再验证、写包先持久预留、无包试运行、旧 trial 跨 boot 阻断与回退测试。仍缺已冻结的真实包分区与 Base provider 装配、同 Flash 写入者独占、实际两份固件身份/状态对账、Base/esp-ota 调用链、产品操作账本、真实断电与 C3 实板的容量/时延/磨损验证。P6-06、P6-08、P6-10、P7-01 因此保持未验收。

## ESP-IDF provider 接线（2026-09-24）

`esp_container_slots_idf.h` 现提供对现有三槽回调的 ESP-IDF 实现。绑定必须由调用方给出已经冻结的**独立**包 data 分区标签、精确地址/大小、三个槽的绝对边界、NVS 分区标签和精确地址/大小、namespace/key，以及与同一设备全部包槽写入者共享的锁。provider 从当前 IDF 分区表发现并读回这些事实；包区只接受可写的 `data/undefined` 分区，NVS 只接受可写的 `data/nvs` 分区，两者不得重叠。错类型、缺失、地址/大小不符、槽未对齐/越界或重叠均在创建回调前拒绝。擦除粒度取实际分区的 `erase_size`；普通写入按 4 字节、加密分区按 IDF 要求的 16 字节对齐，所有 Flash 回调再次检查绝对边界再转为分区相对地址。

锁由调用方创建并覆盖所有包槽操作及运行切换；provider 只做非阻塞获取，不另建私有锁。NVS 必须由产品先初始化，provider 不自动 erase、重建或迁移 NVS；`write_blob` 对一个 key 执行 `set_blob → commit`，`read_blob` 关闭写 handle 后另开只读 handle，精确读取 288 字节并区分 key 缺失、长度错误与 I/O 错误。commit 返回失败即使底层实际写入也仍由三槽引擎按不确定结果裁决。provider 不保存另一份状态，也不负责签名、授权、业务账本或固件身份。

当前 Base 的 4 MiB 分区表没有独立包 data 分区，只有双 app、NVS/otadata/phy/coredump 与末尾 `base_store` NVS；把 `base_store` 当包区会被类型检查拒绝。Container C3 样例只编译本 provider，没有调用它。host 的合成分区/NVS 假件证明缺失/错类型/只读/越界拒绝、绝对到相对 Flash 地址转换、NVS commit/独立读回和三槽初始化/加载；它不是实际 4 MiB 布局证据。P6-03 尚须冻结真实容量与目标分区，P7-01 才能经授权迁移并由 Base 装配共享锁、真实签名/授权和运行绑定。在此之前没有启用安装或运行，P6-08 继续未验收。

## 安装槽到私有运行时的同步连接（2026-09-24）

本轮在 `codex/c3-low-memory` 的 `bbc186ed97fc6c6e8ea4f2b71189560679100caf` 候选上，按已授权的软件范围增加 `src/slot_runtime_internal.h` 的 `econtainer_slot_runtime_open`。它仍为组件私有入口，真实消费者是新增签名 counter 集成测试；没有对外开放裸 Wasm 安装接口，没有接入 Base 物理分区，也没有新增持久相位、活动指针、租约或后台任务。

调用方在同一个 pthread 执行器上先停止回收旧实例，使用 Base 已有的外层 firmware/storage owner 保持固件集合稳定；三槽内层继续使用原有 `io.lock`。请求带实际可启动固件集合、预期 sequence 和选择类型。`CONFIRMED` 只从当前运行固件的实际绑定取包；`TRIAL` 必须精确匹配已提交的 `TRIAL_STARTED`、当前 running firmware、operation ID 和本 boot ID。PREPARED、HEALTH_VERIFIED 或另一 boot 的记录均不能当作新 trial 启动。确认启动仍核对两份可启动固件的所有确认引用，但不要求被抛弃候选完整，保留旧包恢复合同。

包槽引擎在同一锁内读回单 blob、检查 sequence 与固件集合、选择记录中的 slot/整包摘要/长度/ABI/schema，再调用同步装载函数。装载先映射精确整包，使用该只读映射的 ≤512 字节回调完成签名、完整包和 Wasm 摘要、产品、ABI/schema、静态 profile 及独立授权检查，再把同一映射中的精确 Wasm 区间交给私有 `runtime_open`。安装时遗留的 `verified_info` 不作为输入，且无需提供它；确认包复用验证核心，不伪造候选 operation。即使 provider 错误地映射了另一份完整、签名有效的包，持久摘要绑定也会拒绝它。

IDF provider 的 `flash_map/flash_unmap` 是可选的存储回调、必需的私有装载能力。它先检查真实专用分区边界，再使用 `esp_partition_mmap(DATA | BLOCKS_WRITE)`；SDK 负责非页对齐地址和返回指针调整。成功映射无论后续授权或 WAMR 是否成功，均在解锁前解除；映射失败不留下句柄，成功但 NULL 指针也会清理。映射活跃时不执行 NVS commit 或包擦写，避免 `BLOCKS_WRITE` 阻塞自己。`runtime_open` 不执行 guest，并在返回前拥有 WAMR 所需 code/data 副本，因此映射不延长到 `init/event/stop/close` 生命周期。

签名请求与独立 runtime policy 在连接处共同限制 Wasm 大小、内存页、执行栈、三个入口指令预算；能力取 signed requested、静态独立 grant 与运行 policy 的交集，签名不能自行授权。非法 policy 不产生实例。host heap、单事件字节、日志长度、定时器个数和整个入口的 `max_entry_duration_ms` 仍是独立平台参数。截至本段 2026-09-24 检查点，事件队列、持久存储和单次 `host_call_timeout_ms` 尚未进入 runtime。2026-10-04 已将验签后的单次宿主期限与平台正数运行限制取最小值，四导入各自独立计时并在返回时裁决，见[宿主导入检查点](host-api-checkpoint.md)；整个 guest 入口期限仍独立，后置裁决不证明阻塞可抢占或硬返回上界。

返回结构分别保留 `slots` 和 `runtime`：装载前的存储/身份/授权失败使 runtime 保持 `INVALID_STATE`；存储与授权成功但 WAMR 忙或装载失败时，slots 可为 OK、runtime 为具体错误。只有两者均为 OK 且输出实例非 NULL 才能继续 `init`。失败不会给调用方留下部分实例；实际 runtime 内部失败统一回收其资源，连接层负责映射和槽锁，不建立第二套 close 所有权。

普通真实 WAMR CTest **9/9** 通过。新增 `slot_runtime` 使用临时 RSA-3072 测试键签署真实 wasi-sdk 33 counter，执行两个固件绑定下的 P0→P3 更新、确认包恢复、即时 `munmap` 后的 guest 调用、签名配额和独立授权、映射错误与 WAMR 失败清理。两个条件变量同步的 pthread 在 map 后及 open 后分别竞争预留和擦写，均返回 BUSY，写入计数不变；测试没有假定未确认的并发工作模式。具体负例见[测试入口](../../tests/README.md)。测试使用合成 Flash/NVS 和真实 WAMR，不等于真实 Flash 包槽或设备掉电验证。

固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、WAMR `a34d721b630213f59fde0b40cebbb980903660e8` 的独立 C3 构建通过；链接参数额外强制引用 `econtainer_slot_runtime_open`，最终 ELF 中该函数及 `econtainer_slots_with_selected_package`、`econtainer_package_slot_check`、`econtainer_runtime_open` 均为已定义代码符号，避免样例未调用而被回收。app 为 **328,928 字节**（`0x504e0`），SHA-256 `673d896a6e693099c87962c09a89f3b610633a7de527b8a142e52173e55ed0b1`；构建日志在仓外 `/private/tmp/esp-container-slot-runtime.20260924/c3-build.log`。样例没有实际调用新入口，该项只证明完整编译链接，没有写板。

严格 UBSan 复核修正了旧 `slots_test` 假 Flash 的指针表达式：必须先计算 `offset - FLASH_BASE` 再形成数组指针，相关测试假件同步修正。首次 ASan/UBSan 未开启遇错停止时 CTest 虽返回 9/9，但完整日志含恢复性 UB 告警，因此不能作为 UBSan 无告警证据。开启 `UBSAN_OPTIONS=halt_on_error=1` 后，新槽测试和原有裸 `runtime_instance` 均在锁定 WAMR `wasm_interp_classic.c:6835` 的 `WASMBranchBlock` 8 字节对齐处失败；原路径不调用新增槽连接，证明该问题既有。新旧严格失败日志分别保存在 `/private/tmp/esp-container-slot-runtime.20260924/asan-ubsan-strict-tests.log` 与 `/private/tmp/esp-container-slot-audit-baseline-test.20260924.log`，首次恢复性日志为同目录 `asan-recovering-first.log`。本轮不修改 WAMR 依赖或屏蔽对齐检查，完整 UBSan 验证继续未通过。

最终容量与三槽物理几何、生产信任锚/授权、Base 的 pthread 执行器和安装命令、健康窗口、联合 OTA 转换及实板恢复仍未接线。P6-06/P6-08/P6-10 的软件连接取得进展，整项验收状态不变。

独立只读复核未发现新同步连接的阻断缺陷；单独纯 ASan 的 `slots/package_slot/slots_idf/slot_runtime` **4/4** 通过，日志 `/private/tmp/esp-container-slot-audit-address-test.20260924.log`。修正假件指针后，前三项严格 ASan/UBSan **3/3** 通过，日志 `/private/tmp/esp-container-slot-audit-fixture-test.20260924.log`；本轮严格全套为 **6/9**，三个使用 WAMR 的目标均因上述既有对齐 UB 失败。报告分别保留这些结果，不把关闭 UBSan 的 ASan 通过称为联合 sanitizer 通过。

## C3 QEMU 真实 Flash/NVS 装载切片

同轮按明确授权在仓外 `/private/tmp/esp-container-slot-runtime.20260924/qemu-lab` 建立独立 C3 探针，使用上述固定 SDK/WAMR 和官方 QEMU `esp_develop_9.2.2_20260417`。虚拟 4 MiB Flash 中专用 `package_store` 为 `0x110000/0x18000`，划三个 `0x8000` 槽；只有一个 factory app，传入的两个 firmware SHA 是明确标记的 fixture，**并非 Base 从 app/otadata 发现的双固件集合**。这些地址和大小仅为仿真实验，不改变正式分区表，也不冻结业务包容量。虚拟 NVS 初次为空，不使用自动 erase 或失败后重建。

探针从临时 RSA-3072 测试键签署的 **10,240 字节** counter 包写入实际 provider，运行 `reserve → Flash erase/write → readback verify → PREPARED(sequence=3) → durable TRIAL_STARTED → slot_runtime_open → init/event/stop/close → mark_healthy → confirm(sequence=6)`；随后两次从确认绑定重新验签和运行。三个生命周期的事件均返回 `3`。provider 仅由薄计数包装调用真实 `esp_partition_mmap/munmap`；五次成功映射全部解除，每次进入 `init` 前 `active_map=0`。错误产品在真实映射后被拒绝、注入 map 失败不创建映射、已有实例时 WAMR BUSY 不影响旧实例；三条失败之后均可继续运行和关闭。新源码不含这些计数或注入代码。

| 阶段 | 8-bit free / largest（字节） | 8 KiB pthread 最小剩余栈（字节） |
| --- | ---: | ---: |
| pthread 开始，尚未首次建记录/验包 | 310,500 / 172,032 | 6,804 |
| 包准入及错误产品/map 故障检查后 | 309,040 / 172,032 | 3,240 |
| 第一次装载完成 | 227,076 / 114,688 | 3,216 |
| 第一次关闭 | 308,936 / 172,032 | 3,216 |
| 第二、三次确认包装载 | 227,084 / 114,688 | 3,104 |
| 第二、三次关闭 | 308,936 / 172,032 | 3,104 |

包含 guest 存活时第二次验签以触发 BUSY 的全过程最小 free 为 **221,120 字节**；8 KiB pthread 的测得最大已用栈为 **5,088 字节**。三次 close 后 free/largest 相同，首次装载前后仍有 104 字节冷启动差，不能宣称完全零差值；join 后 free 为 **317,388 字节**。该探针没有 Wi-Fi/TLS/FRP/MQTT，也没有 Base 控制链、实板栈保证或跨断电恢复验证，不能外推五组件容量。

未签名的实验 app 为 **376,768 字节**，SHA-256 `ba97686242a7aa74b60baaf35c49c7f3bb165a10ed81c36cbf0cf88310fc2f51`；它嵌入的是已签名测试业务包，二者签名身份不可混称。测试包 SHA-256 为 `2d68842b936826d288e6e922a5fc68af44ed18ff92d1c1ab88d9e9b115d5aff2`，最终串口日志 `qemu-slot-runtime.log` 为 4,369 字节、SHA-256 `51b7025fc06cc759ddaeb3235ed3fe0fd50412aeb36a46b45cf3cfd67511cc6b`。第一次运行也通过，但采集器在 join 行未收全时提前退出，原始输出保留为 `qemu-slot-runtime-first.log`；修正采集器为等待完整行后，从原始空虚拟 Flash 镜像重跑，以上数据来自完整第二次日志。

仓外 `source-freeze.json` 及 `source-snapshot/` 绑定完整组件与测试源码，冻结清单 SHA-256 为 `e30db241d77021f5c8a7729673fa1dee558f398db9ea68e720f6ef5bc43ff75c`；`qemu-receipt.json` 另绑定探针、分区、sdkconfig、依赖锁、包、公钥、app、ELF、初始虚拟 Flash 和日志，SHA-256 为 `241e77e57cc306c1842b5065be583b2432d965e594315888bb55301fcf6bd0b9`。包私钥仅在仓外临时测试目录，生产凭据未使用；没有设备写入、提交或推送。

### 保留虚拟 NVS 的跨 boot 验证

保留上述完整制品，另建 `qemu-reboot-lab` 和 `qemu-trial-seed-lab`。重启 reader 只挂载既有 NVS、调用 `slots_load` 与私有装载接口，不调用 `slots_initialize`、擦除、写包或持久转换；boot fixture 从 `0x55` 改为 `0x66`。宿主只替换虚拟 factory app 区间，NVS、包槽、分区表与 bootloader 原样保留，再启动一个新的 QEMU 进程。因此这是实际虚拟 Flash 跨 boot 读取，不是同一 RAM 结构的再次调用。

第一条路径从前次 `confirmed sequence=6` 的虚拟 Flash 重启，读回同一状态，重新验签装载后 `init/event(result=3)/stop/close` 全部成功，map/unmap 各一次，active map 为零；最后再次读取状态仍为 sequence 6。第二条路径先在明确的 `TRIAL_STARTED sequence=4` commit/readback 后暂停实验线程，再保留虚拟 Flash 重启 reader；新 boot 的 trial 请求返回 CONFLICT，确认选择返回 EMPTY，映射次数为零，持久状态仍为 sequence 4。本种子此前无确认包，因此证明候选不会被自动提升，不能写成已有旧确认包的实板恢复证明。

两条重启路径前后的 **整个 NVS 区和三个包槽区逐字节一致**。确认路径 NVS SHA-256 为 `bf5a9ff0acf12e20652ba6c44e25bef291f61c99889e6663c357f6ff6310e8e2`；各区域和全部输入/输出 Flash 的摘要保存在 `qemu-reboot-receipt.json`，该清单 SHA-256 为 `d96a22c57d2b47f2f137ecbf80a5098a8dec8356dc642f3bc22656010bed54a2`。reader app 为 344,736 字节、SHA-256 `2e23ce10c77fda43f922e9c6215e0bb2fa2b44221a502bc86333efd134368629`；`qemu-confirmed-reboot.log` SHA-256 为 `c2179d7e1642743d7b2dcf28a7520e79e0b78da6afa6cb08aa30f51f1d76c883`，`qemu-trial-reboot.log` 为 `b33ca726316da58358d5236038cc988f85e33ed54b254633bc7aee1f995a9d18`。两个 variant 最初并行配置时曾触发 IDF Component Manager 缓存 `index.lock` 冲突；未删除他方锁，待另一配置完成后串行重试通过，首次环境错误日志保留为 `qemu-trial-seed-build-first.log`。

原始源码冻结点、固定 SDK/WAMR 和测试签名键均未变化。这些新 boot 的固件摘要仍是实验 fixture；没有验证 Base 的真实签名固件身份集合、真实 OTA 回滚、物理板掉电或完整五组件并发。
