# Go 主机验包 SDK

`github.com/esp-space/esp-container/host/productpkg` 提供 Go 1.25+ 的 `product.pkg` v1 验包入口，仅依赖 Go 标准库。独立 module 位于 `host/`，与文档中的历史 QEMU Go fixture、ESP-IDF 工程和浏览器 npm 装配分别构建。Go Server 使用完整 Git 提交解析出的唯一 pseudo-version 消费本模块；不得在消费者仓复制包格式、签名或 Wasm 扫描实现。

```go
import "github.com/esp-space/esp-container/host/productpkg"

spki, err := productpkg.DecodePublicKeyPEM(independentlyConfiguredPublicKeyPEM)
if err != nil { /* 拒绝该信任锚 */ }
verified, err := productpkg.Verify(packageBytes, productpkg.Options{
    PublicKeySPKI: spki,
    ExpectedKeyID: independentlyConfiguredKeyID,
})
if err != nil { /* 不登记、不投递；失败结果为零值 */ }
```

公钥是独立配置的 RSA-3072 SubjectPublicKeyInfo，PEM 入口精确接受单个 `PUBLIC KEY`，拒绝其他材料或额外 PEM。预期 key ID 也由调用方独立提供。SDK 核对规范 ustar 三成员、精确清单字节、签名域、RSA-PSS SHA-256／MGF1-SHA-256／32 字节 salt、完整 Wasm 摘要与同一 Classic ABI 2 静态准入合同；签名盐长由 [Go 官方 `rsa.VerifyPSS`／`PSSOptions`](https://pkg.go.dev/crypto/rsa#VerifyPSS)显式固定。清单数字保留原始 JSON 数字再检查 uint32 值域与规范编码，拒绝布尔、null、指数和越界数；实现使用 [Go 官方 `Decoder.UseNumber`](https://pkg.go.dev/encoding/json#Decoder.UseNumber)。

清单仍最多 4096 字节，Wasm 最多 512 KiB，完整包最多 536576 字节。`MaxWasmBytes`／`MaxPackageBytes` 的零值表示上述默认值，正值只可收紧，完整包下限 10240 字节。实际设备包槽容量由平台独立裁决。ID／完整版本仍为小写连字符 ASCII，未另加 64 字节上限；返回原清单中的 ABI、data schema、capability 与六项配额，以及精确包 SHA-256、包字节数和实际 SPKI SHA-256。

`Verify` 在有界检查后复制包与公钥，返回值不引用输入 buffer；调用方不得在调用期间并发修改输入 slice。返回的是调用方拥有的 Go 值，不保存 SDK 全局可变结果。主机验包结果用于制品登记前的内容核验；设备仍须独立执行平台产品／能力／配额授权、从候选 Flash 回读验签、WAMR 指令装载和试运行确认。静态扫描通过不证明业务运行或安装成功。

`EncodeManifest`／`DecodeManifest` 提供同一规范清单的有界编解码，供已验签元数据的持久化与回读使用；它们仅检查 schema 和规范字节，不产生签名证明或设备授权。登记写入口必须先验证完整包，不能把单独解码的清单当作已验签结果。编码会检查全部字段、数值和 4096 字节上限，失败不返回候选字节。

在本目录执行：

```bash
go test ./...
go test -race ./...
go vet ./...
go test ./productpkg -run='^$' -fuzz='^FuzzVerify$' -fuzztime=15s -parallel=2
go test ./productpkg -run='^$' -fuzz='^FuzzWasmReader$' -fuzztime=15s -parallel=2
go test ./productpkg -run='^$' -fuzz='^FuzzManifestCodec$' -fuzztime=15s -parallel=2
```

`productpkg/testdata/` 保存[公开签名向量](../tests/vectors/product-v1/README.md)的 package／PEM 固定字节，让独立 Go module 归档也可测试；Python 回归逐字节核对它们与唯一公开向量。Go 单测还使用进程内的一次性测试密钥覆盖重签名的错误清单、错误 PSS 参数、长版本与输入归属。它们不构成生产信任锚。仓库根 Python 测试向 Python、浏览器和 Go 提交同一组 37 个独立签名输入，对照接受判断与完整清单／摘要；包括完整 3000 字节版本、清单 4096／4097 字节和 Wasm 512 KiB 边界。Go helper 位于 `tests/`，只用于该跨实现测试。

[包格式与主机工具](../tools/README.md) · [设备流式验包](../docs/operations/package-stream-checkpoint.md)
