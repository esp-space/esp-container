# product.pkg v1 公开签名测试向量

本目录保存同一份真实 RSA-3072/PSS 签名包和两种编码的对应测试公钥。签名仅供验证协议字节；生成时的一次性测试私钥没有入库，也不能用于设备发布或产品授权。

| 文件 | 用途 | SHA-256 |
| --- | --- | --- |
| `product.pkg` | 10,240 字节规范 ustar，按 manifest、signature、Wasm 排序 | `c41930e65d577133a09e7f2105f83faafd63e6d40a3c0af0e3d70aba58907702` |
| `public-key.pem` | 主机验包器使用的 SubjectPublicKeyInfo PEM | `1d6bc6e78d02b4d4924fe122d79a8ab0221df97a9cc38fb4fe1202f103d22d12` |
| `public-key.der` | 设备 C 验包器使用的 PKCS#1 `RSAPublicKey` DER | `347ceb0689593003efa10f5cc07ecaa00b988962a40f5f9410087ceaab340fb1` |

清单由 `examples/counter/spec.example.json` 和 `tests/wasm_fixture.py` 的 171 字节 ABI 2 单页模块生成，精确长度 546 字节。签名输入为 `ESP-CONTAINER-PRODUCT-V1\0` 加清单原始字节，算法为 RSA-3072/PSS、SHA-256、MGF1-SHA-256、32 字节 salt，`signing_key_id` 为 `test-key`。PSS salt 在签名时随机生成；从包中取出既有签名字节，再以固定三成员打包，必须逐字节重现 `product.pkg`。

`tests/test_product_package.py` 用 PEM 校验主机验包、清单和重打包；`tests/package_stream_test.py` 将同一包与 DER 公钥交给 C 的 512 字节只读回调验包器。现有动态签名和负例测试继续验证错误 salt、错误 key、畸形包与读取故障。本向量冻结当前编码与验签参数，不表示已确定设备包槽上限、平台能力授权或生产签名材料。
