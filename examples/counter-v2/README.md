# Counter v2：同固件换业务源码样例

此样例沿用 `counter` 的产品身份、guest ABI 2 和一页 Wasm 内存，只修改事件处理规则：v1 累计事件字节数，v2 累计事件字节值。输入 `{1, 2, 3}` 的返回值分别为 3 和 6；均不访问 GPIO、网络或持久业务数据。

从仓根使用已核对的官方 wasi-sdk 33 构建：

```bash
python3 tools/counter_guest.py build --wasi-sdk "$WASI_SDK_ROOT" \
  --source examples/counter-v2/counter.c --output dist/counter-v2.wasm
python3 tools/counter_guest.py check --wasm dist/counter-v2.wasm
```

`tests/slot_runtime_test.py` 用同一临时测试签名链分别把 v1/v2 源码生成的 Wasm 签为 P1/P2；`slot_runtime` CTest 在同一启动的固件 B 上运行真实包槽验签、试运行、确认和 WAMR 调用，验证 P1 返回 3、P2 返回 6，并继续保留固件 A 的 P0 绑定。这个主机切片不包含设备安装入口、真实 Flash、网络负载或实板换包，因此 P6-11 仍未验收。
