package productpkg

import (
	"bytes"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/json"
	"errors"
	"os"
	"reflect"
	"strings"
	"testing"
)

func vector(t testing.TB) ([]byte, Options) {
	t.Helper()
	data, err := os.ReadFile("testdata/product.pkg")
	if err != nil {
		t.Fatal(err)
	}
	public, err := os.ReadFile("testdata/public-key.pem")
	if err != nil {
		t.Fatal(err)
	}
	spki, err := DecodePublicKeyPEM(public)
	if err != nil {
		t.Fatal(err)
	}
	return data, Options{PublicKeySPKI: spki, ExpectedKeyID: "test-key"}
}

func TestPublicVectorAndOwnedResult(t *testing.T) {
	data, options := vector(t)
	result, err := Verify(data, options)
	if err != nil {
		t.Fatal(err)
	}
	if result.PackageSHA256 != "c41930e65d577133a09e7f2105f83faafd63e6d40a3c0af0e3d70aba58907702" ||
		result.PackageSizeBytes != 10240 || result.Manifest.ProductID != "counter" ||
		result.Manifest.ProductVersion != "v0-1-0" || result.Manifest.GuestABIVersion != 2 ||
		result.Manifest.Limits.MemoryLimitBytes != 65536 || result.Manifest.Payload.SizeBytes != 171 ||
		result.SigningKeyFingerprintSHA256 != hash(options.PublicKeySPKI) {
		t.Fatalf("wrong verified metadata: %+v", result)
	}
	before, _ := json.Marshal(result)
	clear(data)
	clear(options.PublicKeySPKI)
	after, _ := json.Marshal(result)
	if !bytes.Equal(before, after) {
		t.Fatal("result aliases caller input")
	}
}

func TestRejectedBytesTrustAndCeilings(t *testing.T) {
	data, options := vector(t)
	for _, index := range []int{0, 100, 124, 257, 512, 1058, 1536, 2048, 2200, 3072, 10239} {
		copy := bytes.Clone(data)
		copy[index] ^= 1
		if result, err := Verify(copy, options); err == nil || !reflect.DeepEqual(result, VerifiedPackage{}) {
			t.Fatalf("accepted corruption at %d or leaked result: %v", index, err)
		}
	}
	for _, invalid := range [][]byte{nil, data[:511], data[:512], data[:1024], data[:10239], append(bytes.Clone(data), make([]byte, 10240)...)} {
		if _, err := Verify(invalid, options); err == nil {
			t.Fatal("accepted wrong archive length")
		}
	}
	for _, changes := range []Options{
		{PublicKeySPKI: options.PublicKeySPKI, ExpectedKeyID: "wrong"},
		{PublicKeySPKI: options.PublicKeySPKI, ExpectedKeyID: "test-key\n"},
		{PublicKeySPKI: nil, ExpectedKeyID: "test-key"},
		{PublicKeySPKI: make([]byte, 8193), ExpectedKeyID: "test-key"},
		{PublicKeySPKI: []byte("garbage"), ExpectedKeyID: "test-key"},
	} {
		if _, err := Verify(data, changes); !errors.Is(err, ErrInvalidTrustAnchor) {
			t.Fatalf("trust: %v", err)
		}
	}
	for _, limits := range [][2]int{{-1, 0}, {7, 0}, {MaxWasmBytes + 1, 0}, {0, 10239}, {0, MaxPackageBytes + 1}, {170, 0}} {
		changes := options
		changes.MaxWasmBytes, changes.MaxPackageBytes = limits[0], limits[1]
		if _, err := Verify(data, changes); !errors.Is(err, ErrInvalidPackage) {
			t.Fatalf("ceiling: %v", err)
		}
	}
	options.MaxWasmBytes, options.MaxPackageBytes = 171, 10240
	if _, err := Verify(data, options); err != nil {
		t.Fatal("exact ceilings:", err)
	}
}

func TestPEMRejectsAdditionalMaterial(t *testing.T) {
	public, err := os.ReadFile("testdata/public-key.pem")
	if err != nil {
		t.Fatal(err)
	}
	for _, input := range [][]byte{append([]byte("junk\n"), public...), append(bytes.Clone(public), public...),
		append([]byte("-----BEGIN PUBLIC KEY-----\ngarbage\n"), public...),
		append(bytes.Clone(public), []byte("junk")...), []byte("-----BEGIN RSA PUBLIC KEY-----\nAAAA\n-----END RSA PUBLIC KEY-----\n"),
		[]byte("-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n"), make([]byte, 8193)} {
		if _, err := DecodePublicKeyPEM(input); !errors.Is(err, ErrInvalidTrustAnchor) {
			t.Fatal("accepted ambiguous PEM")
		}
	}
}

func signedArchive(t testing.TB, key *rsa.PrivateKey, manifest, wasm []byte, saltLength int) []byte {
	t.Helper()
	domain := append([]byte("ESP-CONTAINER-PRODUCT-V1\x00"), manifest...)
	digest := sha256.Sum256(domain)
	signature, err := rsa.SignPSS(rand.Reader, key, crypto.SHA256, digest[:], &rsa.PSSOptions{SaltLength: saltLength})
	if err != nil {
		t.Fatal(err)
	}
	data := []byte{}
	for i, member := range [][]byte{manifest, signature, wasm} {
		data = append(data, header([]string{"manifest.json", "signature.bin", "app.wasm"}[i], len(member))...)
		data = append(data, member...)
		data = append(data, make([]byte, (512-len(member)%512)%512)...)
	}
	data = append(data, make([]byte, (len(data)+1024+10239)/10240*10240-len(data))...)
	return data
}

func TestIndependentSignaturesManifestAndLongVersion(t *testing.T) {
	data, _ := vector(t)
	members, err := unpack(data, MaxWasmBytes)
	if err != nil {
		t.Fatal(err)
	}
	key, err := rsa.GenerateKey(rand.Reader, 3072)
	if err != nil {
		t.Fatal(err)
	}
	spki, err := x509.MarshalPKIXPublicKey(&key.PublicKey)
	if err != nil {
		t.Fatal(err)
	}
	options := Options{PublicKeySPKI: spki, ExpectedKeyID: "test-key"}
	if _, err := Verify(data, options); !errors.Is(err, ErrSignatureMismatch) {
		t.Fatal("accepted unrelated independently configured RSA-3072 anchor")
	}
	var record map[string]any
	if err := json.Unmarshal(members[0], &record); err != nil {
		t.Fatal(err)
	}
	record["product_version"] = "v" + strings.Repeat("a", 2999)
	manifest, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if result, err := Verify(signedArchive(t, key, manifest, members[2], 32), options); err != nil ||
		len(result.Manifest.ProductVersion) != 3000 {
		t.Fatalf("long version: %v", err)
	}
	for _, salt := range []int{20, rsa.PSSSaltLengthAuto} {
		if _, err := Verify(signedArchive(t, key, manifest, members[2], salt), options); !errors.Is(err, ErrSignatureMismatch) {
			t.Fatalf("salt: %v", err)
		}
	}
	for name, raw := range map[string][]byte{
		"duplicate": bytes.Replace(members[0], []byte(`"product_id":"counter"`), []byte(`"product_id":"wrong","product_id":"counter"`), 1),
		"space":     append([]byte(" "), members[0]...),
		"unknown":   bytes.Replace(members[0], []byte(`"product_id"`), []byte(`"unknown"`), 1),
		"boolean":   bytes.Replace(members[0], []byte(`"data_schema_version":1`), []byte(`"data_schema_version":true`), 1),
		"exponent":  bytes.Replace(members[0], []byte(`"data_schema_version":1`), []byte(`"data_schema_version":1e0`), 1),
		"null":      bytes.Replace(members[0], []byte(`"data_schema_version":1`), []byte(`"data_schema_version":null`), 1),
		"overflow":  bytes.Replace(members[0], []byte(`"data_schema_version":1`), []byte(`"data_schema_version":4294967296`), 1),
		"newline":   bytes.Replace(members[0], []byte(`"product_id":"counter"`), []byte(`"product_id":"counter\n"`), 1),
	} {
		t.Run(name, func(t *testing.T) {
			if bytes.Equal(raw, members[0]) {
				t.Fatal("negative fixture did not change input")
			}
			if _, err := Verify(signedArchive(t, key, raw, members[2], 32), options); !errors.Is(err, ErrInvalidPackage) {
				t.Fatalf("manifest: %v", err)
			}
		})
	}
	smallKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	options.PublicKeySPKI, err = x509.MarshalPKIXPublicKey(&smallKey.PublicKey)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := Verify(data, options); !errors.Is(err, ErrInvalidTrustAnchor) {
		t.Fatal("accepted wrong RSA size")
	}
}

func FuzzVerify(f *testing.F) {
	data, options := vector(f)
	f.Add(data)
	f.Add([]byte{})
	f.Add(make([]byte, 512))
	f.Fuzz(func(t *testing.T, input []byte) {
		result, err := Verify(input, options)
		if err != nil && !reflect.DeepEqual(result, VerifiedPackage{}) {
			t.Fatal("failed verification exposed metadata")
		}
	})
}

func FuzzWasmReader(f *testing.F) {
	data, _ := vector(f)
	members, err := unpack(data, MaxWasmBytes)
	if err != nil {
		f.Fatal(err)
	}
	f.Add(members[2])
	f.Add([]byte{})
	f.Add([]byte{0, 97, 115, 109, 1, 0, 0, 0})
	f.Fuzz(func(t *testing.T, input []byte) {
		if len(input) <= MaxWasmBytes {
			checkWasm(input, Manifest{RequiredCapabilities: []string{"log", "monotonic-time", "timer"}})
		}
	})
}
