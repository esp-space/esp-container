package productpkg

import (
	"bytes"
	"strings"
	"testing"
)

func TestManifestCodecPreservesCanonicalSignedInput(t *testing.T) {
	data, _ := vector(t)
	members, err := unpack(data, MaxWasmBytes)
	if err != nil {
		t.Fatal(err)
	}
	manifest, err := DecodeManifest(members[0])
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := EncodeManifest(manifest)
	if err != nil || !bytes.Equal(encoded, members[0]) {
		t.Fatal("canonical metadata changed:", err)
	}
	manifest.ProductVersion = "v" + strings.Repeat("a", 2999)
	encoded, err = EncodeManifest(manifest)
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := DecodeManifest(encoded)
	if err != nil || decoded.ProductVersion != manifest.ProductVersion {
		t.Fatal("long version changed")
	}
	clear(encoded)
	if decoded.ProductVersion != manifest.ProductVersion {
		t.Fatal("decoded fields alias input")
	}
	manifest.ProductVersion = "v" + strings.Repeat("a", 4096)
	if value, err := EncodeManifest(manifest); err == nil || value != nil {
		t.Fatal("oversized manifest encoded")
	}
	manifest.ProductVersion = "v0-1-0"
	manifest.GuestABIVersion = 3
	if value, err := EncodeManifest(manifest); err == nil || value != nil {
		t.Fatal("unsupported ABI encoded")
	}
	for _, raw := range [][]byte{nil, append([]byte(" "), members[0]...),
		bytes.Replace(members[0], []byte(`"data_schema_version":1`), []byte(`"data_schema_version":true`), 1),
		bytes.Replace(members[0], []byte(`"product_id":"counter"`), []byte(`"product_id":"wrong","product_id":"counter"`), 1)} {
		if _, err := DecodeManifest(raw); err == nil {
			t.Fatal("invalid metadata decoded")
		}
	}
}

func FuzzManifestCodec(f *testing.F) {
	data, _ := vector(f)
	members, err := unpack(data, MaxWasmBytes)
	if err != nil {
		f.Fatal(err)
	}
	f.Add(members[0])
	f.Add([]byte{})
	f.Add([]byte(`{"limits":null}`))
	f.Fuzz(func(t *testing.T, input []byte) {
		manifest, err := DecodeManifest(input)
		if err == nil {
			canonical, err := EncodeManifest(manifest)
			if err != nil || !bytes.Equal(input, canonical) {
				t.Fatal("accepted noncanonical or non-roundtrippable manifest")
			}
		}
	})
}
