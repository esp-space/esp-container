// Package productpkg verifies product.pkg v1 without executing the guest.
// Its trust anchor is supplied independently of the archive. Verification is
// not device authorization or proof that the guest has passed its trial.
package productpkg

import (
	"bytes"
	"crypto"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
)

const (
	ManifestMaxBytes = 4096
	MaxWasmBytes     = 512 * 1024
	MaxPackageBytes  = MaxWasmBytes + ManifestMaxBytes + 8192
	MemoryBytes      = 65536
	EventBufferBytes = 4096
)

var (
	ErrInvalidPackage     = errors.New("invalid_package")
	ErrInvalidTrustAnchor = errors.New("invalid_trust_anchor")
	ErrSignatureMismatch  = errors.New("signature_mismatch")
	identifier            = regexp.MustCompile(`\A[a-z0-9]+(?:-[a-z0-9]+)*\z`)
	digestPattern         = regexp.MustCompile(`\A[0-9a-f]{64}\z`)
	integerPattern        = regexp.MustCompile(`\A(?:0|[1-9][0-9]*)\z`)
	publicPEMPattern      = regexp.MustCompile(`\A-----BEGIN PUBLIC KEY-----\r?\n[A-Za-z0-9+/=\r\n]+-----END PUBLIC KEY-----[ \t\r\n]*\z`)
)

type Limits struct {
	MemoryLimitBytes  uint32 `json:"memory_limit_bytes"`
	StackLimitBytes   uint32 `json:"stack_limit_bytes"`
	EventQueueLimit   uint32 `json:"event_queue_limit"`
	InstructionBudget uint32 `json:"instruction_budget"`
	HostCallTimeoutMS uint32 `json:"host_call_timeout_ms"`
	StorageLimitBytes uint32 `json:"storage_limit_bytes"`
}

type Payload struct {
	Path      string `json:"path"`
	SizeBytes uint32 `json:"size_bytes"`
	SHA256    string `json:"sha256"`
}

type Manifest struct {
	PackageFormatVersion uint32   `json:"package_format_version"`
	ProductID            string   `json:"product_id"`
	ProductVersion       string   `json:"product_version"`
	GuestABIVersion      uint32   `json:"guest_abi_version"`
	RequiredCapabilities []string `json:"required_capabilities"`
	RuntimeProfile       string   `json:"runtime_profile"`
	Limits               Limits   `json:"limits"`
	DataSchemaVersion    uint32   `json:"data_schema_version"`
	Payload              Payload  `json:"payload"`
	SignatureAlgorithm   string   `json:"signature_algorithm"`
	SigningKeyID         string   `json:"signing_key_id"`
}

type VerifiedPackage struct {
	Manifest                    Manifest `json:"manifest"`
	PackageSHA256               string   `json:"package_sha256"`
	PackageSizeBytes            int      `json:"package_size_bytes"`
	SigningKeyFingerprintSHA256 string   `json:"signing_key_fingerprint_sha256"`
}

// Options can only lower the public parsing ceilings; zero selects the default.
type Options struct {
	PublicKeySPKI   []byte
	ExpectedKeyID   string
	MaxWasmBytes    int
	MaxPackageBytes int
}

// DecodePublicKeyPEM accepts exactly one independently supplied PUBLIC KEY.
func DecodePublicKeyPEM(input []byte) ([]byte, error) {
	if len(input) > 8192 || !publicPEMPattern.Match(input) {
		return nil, ErrInvalidTrustAnchor
	}
	block, rest := pem.Decode(input)
	if block == nil || block.Type != "PUBLIC KEY" || len(block.Headers) != 0 ||
		len(bytes.TrimSpace(rest)) != 0 {
		return nil, ErrInvalidTrustAnchor
	}
	// Parse here too: a PEM envelope alone must not confer trust in arbitrary DER.
	if _, err := parseKey(block.Bytes); err != nil {
		return nil, err
	}
	return bytes.Clone(block.Bytes), nil
}

func parseKey(spki []byte) (*rsa.PublicKey, error) {
	if len(spki) == 0 || len(spki) > 8192 {
		return nil, ErrInvalidTrustAnchor
	}
	value, err := x509.ParsePKIXPublicKey(spki)
	if err != nil {
		return nil, ErrInvalidTrustAnchor
	}
	key, ok := value.(*rsa.PublicKey)
	if !ok || key.N.BitLen() != 3072 {
		return nil, ErrInvalidTrustAnchor
	}
	return key, nil
}

// Verify takes owned snapshots of the bounded input and SPKI. Callers must not
// concurrently mutate supplied slices. No input slice is exposed in the result.
func Verify(input []byte, options Options) (VerifiedPackage, error) {
	var zero VerifiedPackage
	maxWasm, maxPackage := options.MaxWasmBytes, options.MaxPackageBytes
	if maxWasm == 0 {
		maxWasm = MaxWasmBytes
	}
	if maxPackage == 0 {
		maxPackage = MaxPackageBytes
	}
	if maxWasm < 8 || maxWasm > MaxWasmBytes || maxPackage < 10240 || maxPackage > MaxPackageBytes ||
		len(input) > maxPackage || len(input)%512 != 0 {
		return zero, ErrInvalidPackage
	}
	if !identifier.MatchString(options.ExpectedKeyID) || len(options.PublicKeySPKI) > 8192 {
		return zero, ErrInvalidTrustAnchor
	}
	data, spki := bytes.Clone(input), bytes.Clone(options.PublicKeySPKI)
	members, err := unpack(data, maxWasm)
	if err != nil {
		return zero, err
	}
	manifest, err := parseManifest(members[0])
	if err != nil {
		return zero, err
	}
	if manifest.SigningKeyID != options.ExpectedKeyID {
		return zero, ErrInvalidTrustAnchor
	}
	key, err := parseKey(spki)
	if err != nil {
		return zero, err
	}
	signed := sha256.New()
	signed.Write([]byte("ESP-CONTAINER-PRODUCT-V1\x00"))
	signed.Write(members[0])
	if err := rsa.VerifyPSS(key, crypto.SHA256, signed.Sum(nil), members[1],
		&rsa.PSSOptions{SaltLength: 32, Hash: crypto.SHA256}); err != nil {
		return zero, ErrSignatureMismatch
	}
	if int(manifest.Payload.SizeBytes) != len(members[2]) || manifest.Payload.SHA256 != hash(members[2]) ||
		!checkWasm(members[2], manifest) {
		return zero, ErrInvalidPackage
	}
	return VerifiedPackage{Manifest: manifest, PackageSHA256: hash(data), PackageSizeBytes: len(data),
		SigningKeyFingerprintSHA256: hash(spki)}, nil
}

func hash(value []byte) string { sum := sha256.Sum256(value); return hex.EncodeToString(sum[:]) }

func header(name string, size int) []byte {
	result := make([]byte, 512)
	copy(result, name)
	for offset, text := range map[int]string{100: "0000644\x00", 108: "0000000\x00", 116: "0000000\x00",
		124: fmt.Sprintf("%011o\x00", size), 136: "00000000000\x00", 257: "ustar\x0000"} {
		copy(result[offset:], text)
	}
	for i := 148; i < 156; i++ {
		result[i] = ' '
	}
	result[156] = '0'
	var checksum int
	for _, value := range result {
		checksum += int(value)
	}
	copy(result[148:], fmt.Sprintf("%06o\x00 ", checksum))
	return result
}

func unpack(data []byte, maxWasm int) ([3][]byte, error) {
	var members [3][]byte
	offset := 0
	for i, name := range []string{"manifest.json", "signature.bin", "app.wasm"} {
		if len(data)-offset < 512 {
			return members, ErrInvalidPackage
		}
		actual := data[offset : offset+512]
		rawSize := string(actual[124:136])
		if len(rawSize) != 12 || rawSize[11] != 0 || strings.Trim(rawSize[:11], "01234567") != "" {
			return members, ErrInvalidPackage
		}
		size, err := strconv.ParseUint(rawSize[:11], 8, 32)
		maximum := []int{ManifestMaxBytes, 384, maxWasm}[i]
		if err != nil || size > uint64(maximum) || !bytes.Equal(actual, header(name, int(size))) {
			return members, ErrInvalidPackage
		}
		if i == 0 && size == 0 || i == 1 && size != 384 || i == 2 && size < 8 {
			return members, ErrInvalidPackage
		}
		offset += 512
		padded := (int(size) + 511) / 512 * 512
		if padded > len(data)-offset {
			return members, ErrInvalidPackage
		}
		members[i] = data[offset : offset+int(size)]
		if !allZero(data[offset+int(size) : offset+padded]) {
			return members, ErrInvalidPackage
		}
		offset += padded
	}
	if len(data) != (offset+1024+10239)/10240*10240 || !allZero(data[offset:]) {
		return members, ErrInvalidPackage
	}
	return members, nil
}

func allZero(data []byte) bool {
	for _, value := range data {
		if value != 0 {
			return false
		}
	}
	return true
}

func parseManifest(data []byte) (Manifest, error) {
	var result Manifest
	if len(data) == 0 || len(data) > ManifestMaxBytes {
		return result, ErrInvalidPackage
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	var raw any
	if err := decoder.Decode(&raw); err != nil {
		return result, ErrInvalidPackage
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return result, ErrInvalidPackage
	}
	canonical, err := json.Marshal(raw)
	if err != nil || !bytes.Equal(data, canonical) {
		return result, ErrInvalidPackage
	}
	root, ok := fields(raw, "package_format_version", "product_id", "product_version", "guest_abi_version",
		"required_capabilities", "runtime_profile", "limits", "data_schema_version", "payload", "signature_algorithm", "signing_key_id")
	if !ok {
		return result, ErrInvalidPackage
	}
	for _, key := range []string{"product_id", "product_version", "runtime_profile", "signing_key_id"} {
		value, ok := root[key].(string)
		if !ok || !identifier.MatchString(value) {
			return result, ErrInvalidPackage
		}
	}
	for _, key := range []string{"package_format_version", "guest_abi_version", "data_schema_version"} {
		if _, ok := number(root[key], 1); !ok {
			return result, ErrInvalidPackage
		}
	}
	if root["package_format_version"] != json.Number("1") || root["guest_abi_version"] != json.Number("2") ||
		root["runtime_profile"] != "wamr-classic-v1" || root["signature_algorithm"] != "rsa-3072-pss-sha256" {
		return result, ErrInvalidPackage
	}
	caps, ok := root["required_capabilities"].([]any)
	if !ok || len(caps) > 16 {
		return result, ErrInvalidPackage
	}
	previous := ""
	for _, item := range caps {
		cap, ok := item.(string)
		if !ok || cap <= previous || cap != "log" && cap != "monotonic-time" && cap != "timer" {
			return result, ErrInvalidPackage
		}
		previous = cap
	}
	limits, ok := fields(root["limits"], "memory_limit_bytes", "stack_limit_bytes", "event_queue_limit", "instruction_budget", "host_call_timeout_ms", "storage_limit_bytes")
	if !ok {
		return result, ErrInvalidPackage
	}
	for key, value := range limits {
		minimum := uint32(1)
		if key == "storage_limit_bytes" {
			minimum = 0
		}
		if _, ok := number(value, minimum); !ok {
			return result, ErrInvalidPackage
		}
	}
	if limits["memory_limit_bytes"] != json.Number("65536") {
		return result, ErrInvalidPackage
	}
	payload, ok := fields(root["payload"], "path", "size_bytes", "sha256")
	if !ok || payload["path"] != "app.wasm" {
		return result, ErrInvalidPackage
	}
	digest, ok := payload["sha256"].(string)
	if !ok || !digestPattern.MatchString(digest) {
		return result, ErrInvalidPackage
	}
	size, ok := number(payload["size_bytes"], 8)
	if !ok || size > MaxWasmBytes {
		return result, ErrInvalidPackage
	}
	if err := json.Unmarshal(data, &result); err != nil {
		return Manifest{}, ErrInvalidPackage
	}
	return result, nil
}

func fields(value any, keys ...string) (map[string]any, bool) {
	object, ok := value.(map[string]any)
	if !ok || len(object) != len(keys) {
		return nil, false
	}
	for _, key := range keys {
		if _, ok := object[key]; !ok {
			return nil, false
		}
	}
	return object, true
}

func number(value any, minimum uint32) (uint32, bool) {
	text, ok := value.(json.Number)
	if !ok || !integerPattern.MatchString(string(text)) {
		return 0, false
	}
	parsed, err := strconv.ParseUint(string(text), 10, 32)
	return uint32(parsed), err == nil && parsed >= uint64(minimum)
}
