"""Cross-check the bounded C reader against host product.pkg v1 bytes."""

from __future__ import annotations

import hashlib
import json
import random
import subprocess
import sys
import tarfile
import tempfile
from io import BytesIO
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import product_package as pkg  # noqa: E402
from wasm_fixture import module, name, section  # noqa: E402


def raw_package(manifest: bytes, signature: bytes, wasm: bytes) -> bytes:
    output = BytesIO()
    with tarfile.open(fileobj=output, mode="w:", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in zip(pkg.MEMBERS, (manifest, signature, wasm), strict=True):
            archive.addfile(pkg._member_info(name, len(data)), BytesIO(data))
    return output.getvalue()


def mutate_header(package: bytes, offset: int, field: slice, value: bytes) -> bytes:
    assert field.stop - field.start == len(value)
    changed = bytearray(package)
    changed[offset + field.start:offset + field.stop] = value
    changed[offset + 148:offset + 156] = b" " * 8
    checksum = sum(changed[offset:offset + 512])
    changed[offset + 148:offset + 156] = f"{checksum:06o}\0 ".encode()
    return bytes(changed)


def main() -> None:
    binary = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        private = root / "private.pem"
        private.write_bytes(key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        public = root / "public.der"
        public.write_bytes(key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        other_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        wrong_public = root / "wrong.der"
        wrong_public.write_bytes(other_key.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.PKCS1))
        spec = json.loads((Path(__file__).resolve().parents[1] /
                           "examples/counter/spec.example.json").read_text())
        wasm = module()
        manifest = pkg.create_manifest(spec, wasm)
        signature = pkg.sign_manifest(manifest, private, "test-key")
        package = pkg.pack(manifest, signature, wasm, max_wasm_bytes=1024)
        package_path = root / "product.pkg"

        def run(data: bytes, *, key_path: Path = public, key_id: str = "test-key",
                cap: int = 1024, fail_at: int | None = None,
                accepted: bool = False) -> str:
            package_path.write_bytes(data)
            args = [str(binary), str(package_path), str(key_path), key_id, str(cap)]
            if fail_at is not None:
                args.append(str(fail_at))
            result = subprocess.run(args, text=True, capture_output=True, check=False)
            assert result.returncode == (0 if accepted else 2), (
                f"unexpected exit {result.returncode}: {result.stdout} {result.stderr}")
            return result.stdout

        valid = run(package, accepted=True)
        assert f"manifest={len(manifest)} wasm_offset=3072 wasm_size={len(wasm)} max_read=512" in valid
        assert hashlib.sha256(package).hexdigest() in valid
        assert hashlib.sha256(wasm).hexdigest() in valid
        vector = Path(__file__).resolve().parent / "vectors/product-v1"
        frozen = (vector / "product.pkg").read_bytes()
        assert hashlib.sha256(frozen).hexdigest() == (
            "c41930e65d577133a09e7f2105f83faafd63e6d40a3c0af0e3d70aba58907702")
        frozen_output = run(frozen, key_path=vector / "public-key.der", accepted=True)
        assert hashlib.sha256(frozen).hexdigest() in frozen_output
        assert f"manifest={len(manifest)} wasm_offset=3072 wasm_size={len(wasm)} max_read=512" in frozen_output
        assert pkg.verify_package(frozen, vector / "public-key.pem", "test-key",
                                  max_wasm_bytes=1024)["product_id"] == "counter"
        assert "result=2" in run(package, key_path=wrong_public)
        run(package, key_id="other-key")
        run(package, cap=7)
        run(package[:-1])
        run(package + bytes(512))
        run(package + bytes(10240))
        for offset in (0, 511, 512, 1024, 1536, 2048, 3072, len(package) - 1):
            assert "result=3" in run(package, fail_at=offset)

        # A valid signature cannot make a malformed archive or manifest acceptable.
        changed = bytearray(package)
        changed[3072] ^= 1
        run(bytes(changed))
        changed = bytearray(package)
        changed[512 + len(manifest)] = 1
        run(bytes(changed))
        changed = bytearray(package)
        changed[-1] = 1
        run(bytes(changed))
        run(mutate_header(package, 0, slice(0, 100), b"../manifest.json".ljust(100, b"\0")))
        run(mutate_header(package, 0, slice(156, 157), b"2"))
        run(mutate_header(package, 2560, slice(124, 136), b"77777777777\0"))
        run(mutate_header(package, 2560, slice(500, 501), b"x"))
        changed = bytearray(package)
        changed[148] ^= 1
        run(bytes(changed))

        wrong_salt = key.sign(pkg.DOMAIN + manifest,
                              padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                          salt_length=padding.PSS.MAX_LENGTH),
                              hashes.SHA256())
        run(raw_package(manifest, wrong_salt, wasm))

        def signed_invalid(changed_manifest: bytes) -> None:
            changed_signature = key.sign(pkg.DOMAIN + changed_manifest,
                                         padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                                     salt_length=32), hashes.SHA256())
            run(raw_package(changed_manifest, changed_signature, wasm))

        signed_invalid(manifest + b" ")
        signed_invalid(manifest[:-1] + b',"product_id":"counter"}')
        size = str(len(wasm)).encode("ascii")
        signed_invalid(manifest.replace(b'"size_bytes":' + size,
                                        b'"size_bytes":0' + size))
        signed_invalid(manifest.replace(b'"package_format_version":1',
                                        b'"package_format_version":2'))
        signed_invalid(manifest.replace(b'"app.wasm"', b'"../app.wasm"'))
        signed_invalid(manifest.replace(b'"signing_key_id":"test-key"',
                                        b'"signing_key_id":"other-key"'))
        signed_invalid(manifest.replace(b'"required_capabilities":[]',
                                        b'"required_capabilities":["z","a"]'))
        signed_invalid(manifest.replace(b'"data_schema_version":1',
                                        b'"data_schema_version":4294967296'))

        generator = random.Random(0)
        for _ in range(64):
            changed = bytearray(package)
            index = generator.randrange(len(changed))
            changed[index] ^= 1 << generator.randrange(8)
            run(bytes(changed))

        # A large, valid custom section proves payload hashing uses 512-byte reads.
        long_wasm = wasm + section(0, name("name") + bytes(65536))
        long_manifest = pkg.create_manifest(spec, long_wasm)
        long_signature = pkg.sign_manifest(long_manifest, private, "test-key")
        long_package = pkg.pack(long_manifest, long_signature, long_wasm,
                                max_wasm_bytes=70000)
        assert "max_read=512" in run(long_package, cap=70000, accepted=True)
    print("package_stream: valid, signed-invalid, archive-invalid, I/O fault and 64 KiB payload passed")


if __name__ == "__main__":
    main()
