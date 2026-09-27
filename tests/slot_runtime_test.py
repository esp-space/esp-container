"""Run actual signed counter packages from slots on the locked Classic engine."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

from package_wasm_test import signed_package


def main() -> None:
    binary, guests = Path(sys.argv[1]), Path(sys.argv[2])
    root = Path(__file__).resolve().parents[1]
    spec = json.loads((root / "examples/counter/spec.example.json").read_text())
    counter = (guests / "counter.wasm").read_bytes()
    counter_v2 = (guests / "counter-v2.wasm").read_bytes()
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = temporary / "private.pem"
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        (temporary / "public.der").write_bytes(key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        for index in range(4):
            current = copy.deepcopy(spec)
            current["product_version"] = f"v0-1-{index}"
            wasm = counter if index < 2 else counter_v2
            (temporary / f"p{index}.pkg").write_bytes(signed_package(private, wasm, current))
        for name, field, limit in (("budget", "instruction_budget", 1),
                                   ("stack", "stack_limit_bytes", 8)):
            current = copy.deepcopy(spec)
            current["limits"][field] = limit
            (temporary / f"{name}.pkg").write_bytes(signed_package(private, counter, current))
        current = copy.deepcopy(spec)
        current["required_capabilities"] = ["log", "monotonic-time"]
        (temporary / "host.pkg").write_bytes(signed_package(
            private, (guests / "host-api.wasm").read_bytes(), current))
        current = copy.deepcopy(spec)
        current["required_capabilities"] = ["timer"]
        (temporary / "timer.pkg").write_bytes(signed_package(
            private, (guests / "timer.wasm").read_bytes(), current))
        # The static scanner intentionally leaves UTF-8/custom-section loader
        # validation to WAMR. Admission succeeds; the real loader must fail cleanly.
        (temporary / "bad-loader.pkg").write_bytes(signed_package(
            private, counter + b"\x00\x02\x01\xff", spec))
        subprocess.run([str(binary), str(temporary)], check=True)


if __name__ == "__main__":
    main()
