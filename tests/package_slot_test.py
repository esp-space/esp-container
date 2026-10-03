"""Exercise a real signed package through the reserved Flash slot readback path."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_wasm_test import module, signed_package  # noqa: E402


def main() -> None:
    binary = Path(sys.argv[1])
    root = Path(__file__).resolve().parents[1]
    spec = json.loads((root / "examples/counter/spec.example.json").read_text())
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = temporary / "private.pem"
        private.write_bytes(key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        public = temporary / "public.der"
        public.write_bytes(key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        package = temporary / "product.pkg"
        package.write_bytes(signed_package(private, module(), spec))
        for mode in ("valid", "reuse", "changed-copy", "product", "schema", "key-id", "memory", "queue",
                     "budget", "timeout", "read-fault", "wasm-read-fault"):
            result = subprocess.run([str(binary), str(package), str(public), mode],
                                    capture_output=True, text=True, check=False)
            assert result.returncode == 0, f"{mode}: {result.stdout} {result.stderr}"
            if mode in ("valid", "changed-copy"):
                expected = "result=0 phase=2"
            elif mode == "reuse":
                expected = "result=0 phase=5"
            elif mode in ("read-fault", "wasm-read-fault"):
                expected = "result=3 phase=1"
            else:
                expected = "result=8 phase=1"
            assert expected in result.stdout, f"{mode}: {result.stdout}"
        reference = temporary / "reference.pkg"
        reference.write_bytes(package.read_bytes())
        for mode, version, identical in (
            ("identity-conflict", spec["product_version"], False),
            ("identity-rollback-conflict", spec["product_version"], False),
            ("identity-same-sha", spec["product_version"], True),
            ("identity-same-sha-schema-conflict", spec["product_version"], True),
            ("identity-new-version", "v0-1-1", False),
        ):
            changed = dict(spec, product_version=version)
            package.write_bytes(reference.read_bytes() if identical else signed_package(private, module(), changed))
            result = subprocess.run([str(binary), str(package), str(public), mode, str(reference)],
                                    capture_output=True, text=True, timeout=5, check=False)
            assert result.returncode == 0, f"{mode}: {result.stdout} {result.stderr}"
        long_version = "v" + "a" * 2048
        cases = (
            ("identity-conflict", long_version, long_version, {}),
            ("identity-long-tail", long_version, long_version[:-1] + "b", {}),
            ("identity-long-length", long_version, long_version[:-1], {}),
            ("identity-read-fault", spec["product_version"], "v0-1-1", {}),
            ("identity-schema-conflict", spec["product_version"], "v0-1-1", {"data_schema_version": 2}),
            ("identity-rollback-old-abi", spec["product_version"], "v0-1-1", {"guest_abi_version": 3}),
        )
        for mode, old_version, new_version, old_changes in cases:
            old_spec = dict(spec, product_version=old_version, **old_changes)
            reference.write_bytes(signed_package(private, module(), old_spec))
            package.write_bytes(signed_package(private, module(), dict(spec, product_version=new_version)))
            result = subprocess.run([str(binary), str(package), str(public), mode, str(reference)],
                                    capture_output=True, text=True, timeout=5, check=False)
            assert result.returncode == 0, f"{mode}: {result.stdout} {result.stderr}"
        # The reference's persistent digest agrees with these damaged bytes;
        # full signature/manifest validation must still reject it.
        damaged = bytearray(signed_package(private, module(), spec))
        damaged[512] ^= 1
        reference.write_bytes(damaged)
        package.write_bytes(signed_package(private, module(), dict(spec, product_version="v0-1-1")))
        result = subprocess.run([str(binary), str(package), str(public), "identity-corrupt-conflict", str(reference)],
                                capture_output=True, text=True, timeout=5, check=False)
        assert result.returncode == 0, f"damaged reference: {result.stdout} {result.stderr}"
        reference.write_bytes(signed_package(private, module(), dict(spec, product_id="another-product")))
        result = subprocess.run([str(binary), str(package), str(public), "identity-another-product", str(reference)],
                                capture_output=True, text=True, timeout=5, check=False)
        assert result.returncode == 0, f"another product: {result.stdout} {result.stderr}"
        package.write_bytes(signed_package(private, module(), spec))
        reference.write_bytes(signed_package(private, module(), spec))
        for mode in ("retired-conflict", "retired-corrupt"):
            if mode == "retired-corrupt":
                damaged = bytearray(reference.read_bytes())
                damaged[512] ^= 1
                reference.write_bytes(damaged)
            result = subprocess.run([str(binary), str(package), str(public), mode, str(reference)],
                                    capture_output=True, text=True, timeout=5, check=False)
            assert result.returncode == 0, f"{mode}: {result.stdout} {result.stderr}"
    print("package_slot: signed Flash readback, policy, schema and read faults passed")


if __name__ == "__main__":
    main()
