# product.pkg v1 只读流式验包切片

`esp_container_package_verify()` 接受由调用方提供的只读随机访问回调、精确包长、独立信任的 RSA-3072 PKCS#1 `RSAPublicKey` DER 公钥与对应 `signing_key_id`。组件每次最多读取 512 字节；调用方提供 4096 字节 manifest、384 字节签名和 512 字节读取块工作区，设备不把整包复制到 RAM。失败时结果清零。当前 host 与设备共同使用的 512 KiB Wasm 扫描上限只是临时拒绝边界，不能当作 4 MiB C3 的已验证包槽预算。

验包成功后，`econtainer_package_info_t` 除包摘要与 Wasm 偏移外，还返回已签名的 `guest_abi_version`、`data_schema_version`、Classic profile、全部六项资源请求和能力声明；产品 ID/版本以 `workspace->manifest` 中的有界偏移和长度提供，只有在该 workspace 保持原样时有效。它们**只是包的请求**。`econtainer_package_wasm_check()` 必须使用相同、稳定的包字节和上一步的 info，同时接收平台独立提供的 `econtainer_wasm_authorization_t`；把 info 中的能力或资源请求直接复制成授权会使检查失效。接口每次读取最多 512 字节，不分配整包或整份 Wasm 的 RAM，不写入存储，也不启动 guest。

`econtainer_package_slot_validate()` 是三槽 `write_and_prepare` 的真实候选回读回调。三槽引擎先比对完整包 SHA-256，再在同一共享锁下从候选 Flash 回读，验签并重新扫描 Wasm；回调将已签名产品 ID 与平台授权产品比较，将包摘要、ABI 和数据 schema 与持久候选操作比较，并将队列、指令、宿主期限和存储请求与调用方独立限额比较。内存、栈与能力继续由 Wasm 检查器比较；失败不进入 PREPARED，输出 info 清零。验签或 Wasm 二次读取失败透传为 `IO_FAILED`，完整读到却被签名、格式或授权拒绝返回 `UNTRUSTED`，避免将 Flash 故障误报为不可信包。产品版本通过精确包摘要绑定，回调不将该字段解释为安装授权。测试用真实临时 RSA 签名包、假 Flash/NVS 和多项授权负例覆盖此组合。

解析器只接受 host `product_package.py` 生成的规范无压缩 ustar：固定三成员与顺序，逐字节固定 header，零填充、两个结束块与精确 10 KiB record 长度。manifest 按规范 JSON 的固定键顺序和类型读取，拒绝重复/未知字段、非规范编码、越界整数、非小写十六进制摘要和不匹配的 key ID。签名覆盖 `ESP-CONTAINER-PRODUCT-V1\0` 与 manifest 精确字节，RSA-PSS 固定 SHA-256、MGF1-SHA-256、32 字节 salt；公钥不从包中读取。Wasm 按块计算 SHA-256，核对清单长度和摘要；整个归档也按流计算 SHA-256，供上层绑定请求。

2026-09-27 又加入[公开固定签名向量](../../tests/vectors/product-v1/README.md)：10,240 字节真实签名包及同一测试公钥的 PEM／PKCS#1 DER 编码。主机和 C 流式验包回归直接读取这份不可变输入，核对精确 SHA-256、清单、ABI 2 Wasm、签名、512 字节读取上限和重打包字节；临时私钥仅用于生成该向量，未提交。本向量补齐跨实现的公开字节证据，不代替独立产品授权、包槽容量或实体设备验收。

host 测试由主机打包器生成真实临时签名包，以另一个公钥、错误 PSS salt、已签名畸形清单、路径/类型/长度/扩展字段、填充/尾部损坏、截断、不同读取偏移的故障，以及 64 KiB 载荷交叉验证 C 解析器。测试不使用生产密钥，也不写设备。`tests/package_stream_test.py` 通过 CTest 运行。

静态检查接受 ABI v1 与 `wamr-classic-v1`，只允许精确 `econtainer.monotonic_ms() -> i64`、`econtainer.log(i32,i32) -> i32`、`econtainer.timer_start(i32,i32) -> i64` 和 `econtainer.timer_cancel(i64) -> i32` 函数导入，分别要求签名清单声明 `monotonic-time`、`log` 或 `timer`；任一声明还必须落在平台可信能力集合内。重复/未知导入、错误签名、未声明导入和未知能力都拒绝。导出恰好为互不复用的 `econtainer_init() -> i32`、`econtainer_on_event(i32,i32) -> i32`、`econtainer_stop() -> i32` 及非共享 `memory`。Wasm 内存必须有上限且不超过清单需求与平台内存授权，栈需求也不得超过平台授权。扫描拒绝 start、table/element、`target_features`、坏 LEB、截断/乱序/重复 section、越界索引和代码数量不符；WAMR loader 仍负责最终指令与剩余 Wasm 语义验证。扫描尾部再次按流计算 Wasm SHA-256，核对验包结果中的摘要。主机 `product_package.py` 在清单生成、打包和验包时执行相同的可由包字节确定的 Classic/ABI 初筛，但不代替设备的独立授权、Flash 回读和 WAMR 加载。

固定 wasi-sdk 33 的主机交叉测试使用真实签名包，对照 host `product_package._wasm()` 的无导入/单导入/双导入结果；同时使用真实编译的 counter 与双宿主导入 guest，以及独立授权不足、畸形 ABI/LEB/section/重复导入、读取失败和二次读取摘要不符。Python unittest 18/18，普通与 ASan/UBSan CTest 各 3/3，锁定 WAMR 的主机 CTest 5/5 通过。公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 的 C3 样例原样编译通过。未提交的临时 QEMU 探针在官方 Espressif 9.2.2 C3 仿真器中读取镜像内嵌的 10,240 字节测试签名包，回报 `signed_wasm package=0 positive=0 denied=3`；临时镜像 328,608 字节，SHA-256 `59c703c9ef228317f8bbc119aaf68ae0822b80fbda9a1d0e0a9ce5556edc8272`。该探针的测试私钥和源码注入未提交，运行后已删除。

固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与公开 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的 C3 样例完成编译。独立临时链接/运行探针在仓外生成 10,240 字节测试签名包、临时 RSA-3072 测试公钥和错误 salt 签名，再临时嵌入样例；官方 Espressif QEMU 9.2.2 的二进制 SHA-256 为 `3e38982c1ea3e750edfc8c910a0fd44727fe07d9c666b84d18d2b7985ac58246`。设备端 PSA 路径回报 `package_pss_positive=0 wrong_salt=2`，分别对应验包通过与不可信签名；同次 WAMR 样例回报 `normal=1 instruction_limit=1`。临时镜像大小为 `0x4f620`、SHA-256 为 `76bea69cbe66970eef1d4fa8f1184b5e6271ac03b13bf275127e2b41f9934704`。独立样例不链接验包入口时镜像为 `0x390d0`；这约 90 KiB 的差值只反映该独立链接探针，不能直接外推五组件增量或正式分区余量。测试包、公钥与入口改动均未进入仓库提交。

只读验包和静态检查本身不安装或启动业务。QEMU 正向测试读取的是临时镜像内的常量包；新增回读接线在假 Flash 上运行，尚未接当前 Base 的真实包分区。P6-06 仍需在 P6-03/P7-01 确定的真实候选包槽和 Base 共享锁下验证稳定 Flash 字节、独立可信产品授权、签名 key 来源、所有写入者排他及激活前内容绑定；宿主调用期限等请求还须由实际运行期执行，不能因准入时比较数值就宣称墙钟约束已生效。没有完整装配时不得把 PREPARED 当作业务可运行或设备安全启动。
