package productpkg

import (
	"bytes"
	"encoding/json"
)

// DecodeManifest 只解码规范 v1 清单，不验签或授予产品权限。
// 持久登记的写入口必须先对完整包调用 Verify。
func DecodeManifest(input []byte) (Manifest, error) { return parseManifest(input) }

// EncodeManifest 校验并产生规范 v1 清单字节；编码不产生签名证明。
func EncodeManifest(value Manifest) ([]byte, error) {
	raw, err := json.Marshal(value)
	if err != nil {
		return nil, ErrInvalidPackage
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var object any
	if err := decoder.Decode(&object); err != nil {
		return nil, ErrInvalidPackage
	}
	canonical, err := json.Marshal(object)
	if err != nil {
		return nil, ErrInvalidPackage
	}
	if _, err := parseManifest(canonical); err != nil {
		return nil, err
	}
	return canonical, nil
}
