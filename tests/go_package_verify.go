// Test-only cross-language adapter; build from the repository's host module.
package main

import (
	"encoding/json"
	"os"

	"github.com/esp-space/esp-container/host/productpkg"
)

type input struct {
	Package      []byte `json:"package"`
	PublicKeyPEM string `json:"public_key_pem"`
	KeyID        string `json:"key_id"`
}
type output struct {
	Accepted bool                        `json:"accepted"`
	Verified *productpkg.VerifiedPackage `json:"verified"`
}

func main() {
	var inputs []input
	if err := json.NewDecoder(os.Stdin).Decode(&inputs); err != nil {
		os.Exit(1)
	}
	outputs := make([]output, len(inputs))
	for i, item := range inputs {
		spki, err := productpkg.DecodePublicKeyPEM([]byte(item.PublicKeyPEM))
		if err != nil {
			continue
		}
		result, err := productpkg.Verify(item.Package, productpkg.Options{PublicKeySPKI: spki, ExpectedKeyID: item.KeyID})
		if err == nil {
			outputs[i] = output{Accepted: true, Verified: &result}
		}
	}
	if err := json.NewEncoder(os.Stdout).Encode(outputs); err != nil {
		os.Exit(1)
	}
}
