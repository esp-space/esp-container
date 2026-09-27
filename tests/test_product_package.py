from __future__ import annotations

import copy
import hashlib
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import product_package as pkg  # noqa: E402
from wasm_fixture import module, name, section  # noqa: E402


WASM = module()
SPEC = {
    "product_id": "counter",
    "product_version": "v0-1-0",
    "guest_abi_version": 2,
    "required_capabilities": [],
    "runtime_profile": "wamr-classic-v1",
    "limits": {
        "memory_limit_bytes": 65536,
        "stack_limit_bytes": 4096,
        "event_queue_limit": 8,
        "instruction_budget": 100000,
        "host_call_timeout_ms": 100,
        "storage_limit_bytes": 0,
    },
    "data_schema_version": 1,
    "signing_key_id": "test-key",
}


class ProductPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.private = Path(cls.temp.name) / "private.pem"
        cls.public = Path(cls.temp.name) / "public.pem"
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.private.write_bytes(key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        cls.public.write_bytes(key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def _package(self, wasm: bytes = WASM) -> tuple[bytes, bytes, bytes]:
        manifest = pkg.create_manifest(SPEC, wasm)
        signature = pkg.sign_manifest(manifest, self.private, "test-key")
        package = pkg.pack(manifest, signature, wasm, max_wasm_bytes=1024)
        return manifest, signature, package

    @staticmethod
    def _header_field(package: bytes, offset: int, start: int, end: int,
                      replacement: bytes) -> bytes:
        assert len(replacement) == end - start
        changed = bytearray(package)
        changed[offset + start:offset + end] = replacement
        changed[offset + 148:offset + 156] = b" " * 8
        checksum = sum(changed[offset:offset + 512])
        changed[offset + 148:offset + 156] = f"{checksum:06o}\0 ".encode("ascii")
        return bytes(changed)

    @staticmethod
    def _member_offsets(manifest: bytes) -> tuple[int, int, int]:
        signature_header = 512 + ((len(manifest) + 511) // 512) * 512
        wasm_header = signature_header + 512 + 512
        archive_end = wasm_header + 512 + 512
        return signature_header, wasm_header, archive_end

    def test_signed_round_trip_and_deterministic_pack(self) -> None:
        manifest, signature, package = self._package()
        self.assertEqual(pkg.verify_package(package, self.public, "test-key", max_wasm_bytes=1024)["product_id"], "counter")
        self.assertEqual(package, pkg.pack(manifest, signature, WASM, max_wasm_bytes=1024))
        self.assertEqual(pkg.unpack(package, max_wasm_bytes=1024), (manifest, signature, WASM))

    def test_fixed_host_encoding_vector(self) -> None:
        # 固定字节只检查 host 编码，不冻结发布密钥，也不依赖 RSA-PSS 的随机 salt。
        manifest = pkg.create_manifest(SPEC, WASM)
        signature = bytes(range(256)) + bytes(range(128))
        package = pkg.pack(manifest, signature, WASM, max_wasm_bytes=1024)
        self.assertEqual(len(manifest), 546)
        self.assertEqual(hashlib.sha256(manifest).hexdigest(),
                         "c64145a7caf097fc29451a8616daa8ad56ff738a9134a5e3d76d4dbe971153d4")
        self.assertEqual(len(package), 10240)
        self.assertEqual(hashlib.sha256(package).hexdigest(),
                         "8ef5cd8d34db16c5d8919412cbd6c69c0468d696b5a61726f1eb746768e2d71a")
        self.assertEqual(pkg.unpack(package, max_wasm_bytes=1024),
                         (manifest, signature, WASM))

    def test_public_signed_vector(self) -> None:
        vector = Path(__file__).resolve().parent / "vectors/product-v1"
        package = (vector / "product.pkg").read_bytes()
        self.assertEqual(hashlib.sha256(package).hexdigest(),
                         "c41930e65d577133a09e7f2105f83faafd63e6d40a3c0af0e3d70aba58907702")
        self.assertEqual(hashlib.sha256((vector / "public-key.der").read_bytes()).hexdigest(),
                         "347ceb0689593003efa10f5cc07ecaa00b988962a40f5f9410087ceaab340fb1")
        manifest, signature, wasm = pkg.unpack(package, max_wasm_bytes=1024)
        self.assertEqual(manifest, pkg.create_manifest(SPEC, WASM))
        self.assertEqual(wasm, WASM)
        self.assertEqual(len(signature), pkg.SIGNATURE_BYTES)
        self.assertEqual(package, pkg.pack(manifest, signature, wasm, max_wasm_bytes=1024))
        self.assertEqual(pkg.verify_package(package, vector / "public-key.pem", "test-key",
                                            max_wasm_bytes=1024)["product_id"], "counter")

    def test_payload_mutation_rejected(self) -> None:
        _, _, package = self._package()
        changed = bytearray(package)
        changed[3072] ^= 1
        with self.assertRaisesRegex(pkg.PackageError, "摘要|Wasm"):
            pkg.verify_package(bytes(changed), self.public, "test-key", max_wasm_bytes=1024)

    def test_manifest_mutation_rejected_by_signature(self) -> None:
        manifest, signature, _ = self._package()
        changed = manifest.replace(b"counter", b"counted")
        package = pkg.pack(changed, signature, WASM, max_wasm_bytes=1024)
        with self.assertRaisesRegex(pkg.PackageError, "签名"):
            pkg.verify_package(package, self.public, "test-key", max_wasm_bytes=1024)

    def test_wrong_key_rejected(self) -> None:
        _, _, package = self._package()
        other = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        path = Path(self.temp.name) / "other.pem"
        path.write_bytes(other.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        with self.assertRaisesRegex(pkg.PackageError, "签名"):
            pkg.verify_package(package, path, "test-key", max_wasm_bytes=1024)
        with self.assertRaisesRegex(pkg.PackageError, "key ID"):
            pkg.verify_package(package, self.public, "other-key", max_wasm_bytes=1024)
        with self.assertRaisesRegex(pkg.PackageError, "key ID"):
            pkg.sign_manifest(pkg.create_manifest(SPEC, WASM), self.private, "other-key")

    def test_wrong_pss_parameters_and_signature_mutation_rejected(self) -> None:
        manifest, _, package = self._package()
        signature_header, _, _ = self._member_offsets(manifest)
        changed = bytearray(package)
        changed[signature_header + 512] ^= 1
        with self.assertRaisesRegex(pkg.PackageError, "签名验证失败"):
            pkg.verify_package(bytes(changed), self.public, "test-key", max_wasm_bytes=1024)

        key = serialization.load_pem_private_key(self.private.read_bytes(), password=None)
        wrong_salt = key.sign(pkg.DOMAIN + manifest,
                              padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                          salt_length=padding.PSS.MAX_LENGTH),
                              hashes.SHA256())
        wrong_package = pkg.pack(manifest, wrong_salt, WASM, max_wasm_bytes=1024)
        with self.assertRaisesRegex(pkg.PackageError, "签名验证失败"):
            pkg.verify_package(wrong_package, self.public, "test-key", max_wasm_bytes=1024)

    def test_duplicate_json_key_and_unknown_field_rejected(self) -> None:
        manifest, _, _ = self._package()
        with self.assertRaisesRegex(pkg.PackageError, "重复"):
            pkg._manifest(manifest[:-1] + b',"product_id":"counter"}')
        value = json.loads(manifest)
        value["unknown"] = True
        with self.assertRaisesRegex(pkg.PackageError, "未知"):
            pkg._manifest(pkg._json_bytes(value))
        size = str(len(WASM)).encode("ascii")
        with self.assertRaisesRegex(pkg.PackageError, "重复"):
            pkg._manifest(manifest.replace(b'"size_bytes":' + size,
                                           b'"size_bytes":' + size + b',"size_bytes":' + size))

    def test_manifest_schema_bounds_rejected(self) -> None:
        manifest, _, _ = self._package()
        base = json.loads(manifest)
        changes = (
            ("包格式", lambda value: value.update(package_format_version=2)),
            ("字段", lambda value: value["limits"].update(unknown_limit=1)),
            ("字段", lambda value: value["payload"].update(extra_path="app.wasm")),
            ("guest_abi_version", lambda value: value.update(guest_abi_version=0x100000000)),
            ("data_schema_version", lambda value: value.update(data_schema_version=0)),
            ("required_capabilities", lambda value: value.update(required_capabilities=["gpio", "gpio"])),
            ("限制", lambda value: value["limits"].update(instruction_budget=0)),
            ("限制", lambda value: value["limits"].update(instruction_budget=0x100000000)),
            ("限制", lambda value: value["limits"].update(host_call_timeout_ms=True)),
            ("限制", lambda value: value["limits"].update(storage_limit_bytes=-1)),
            ("payload 长度", lambda value: value["payload"].update(size_bytes=True)),
            ("payload 路径", lambda value: value["payload"].update(path="../app.wasm")),
            ("签名算法", lambda value: value.update(signature_algorithm="rsa-3072-pkcs1-sha256")),
        )
        for error, mutate in changes:
            with self.subTest(error=error, mutation=mutate):
                value = copy.deepcopy(base)
                mutate(value)
                with self.assertRaisesRegex(pkg.PackageError, error):
                    pkg._manifest(pkg._json_bytes(value))

    def test_start_and_truncated_wasm_rejected(self) -> None:
        with self.assertRaisesRegex(pkg.PackageError, "start"):
            pkg.create_manifest(SPEC, WASM + b"\x08\x01\x00")
        with self.assertRaisesRegex(pkg.PackageError, "截断"):
            pkg.create_manifest(SPEC, WASM + b"\x01\x05\x00")
        with self.assertRaisesRegex(pkg.PackageError, "ABI"):
            pkg.create_manifest(SPEC, module(duplicate_export=True))

    def test_exact_host_imports_require_manifest_capabilities(self) -> None:
        wasm = module(("monotonic_ms", "log"))
        self.assertEqual(pkg._wasm(wasm), frozenset({"log", "monotonic-time"}))
        with self.assertRaisesRegex(pkg.PackageError, "授权"):
            pkg.create_manifest(SPEC, wasm)
        spec = copy.deepcopy(SPEC)
        spec["required_capabilities"] = ["log", "monotonic-time"]
        manifest = pkg.create_manifest(spec, wasm)
        signature = pkg.sign_manifest(manifest, self.private, "test-key")
        package = pkg.pack(manifest, signature, wasm, max_wasm_bytes=1024)
        self.assertEqual(pkg.verify_package(package, self.public, "test-key",
                                            max_wasm_bytes=1024)["required_capabilities"],
                         ["log", "monotonic-time"])
        with self.assertRaisesRegex(pkg.PackageError, "imports"):
            pkg._wasm(wasm.replace(b"monotonic_ms", b"monotonic_us"))
        with self.assertRaisesRegex(pkg.PackageError, "imports"):
            pkg._wasm(wasm.replace(b"\x60\x00\x01\x7e", b"\x60\x00\x01\x7f"))

    def test_tar_member_and_tail_rejected(self) -> None:
        manifest, _, package = self._package()
        signature_header, wasm_header, archive_end = self._member_offsets(manifest)
        changed = bytearray(package)
        changed[:8] = b"evil.bin"
        with self.assertRaises(pkg.PackageError):
            pkg.unpack(bytes(changed), max_wasm_bytes=1024)
        for offset, name in ((signature_header, b"manifest.json"),
                             (wasm_header, b"../app.wasm"),
                             (wasm_header, b"/app.wasm")):
            with self.subTest(member=name):
                changed = self._header_field(package, offset, 0, 100,
                                             name + bytes(100 - len(name)))
                with self.assertRaisesRegex(pkg.PackageError, "成员名称"):
                    pkg.unpack(changed, max_wasm_bytes=1024)
        extra = bytearray(package)
        extra[archive_end:archive_end + 512] = pkg._member_info("extra.bin", 0).tobuf(
            format=tarfile.USTAR_FORMAT)
        with self.assertRaisesRegex(pkg.PackageError, "归档结束块"):
            pkg.unpack(bytes(extra), max_wasm_bytes=1024)
        changed = bytearray(package)
        changed[-1] = 1
        with self.assertRaisesRegex(pkg.PackageError, "尾随"):
            pkg.unpack(bytes(changed), max_wasm_bytes=1024)
        with self.assertRaisesRegex(pkg.PackageError, "尾随"):
            pkg.verify_package(package + bytes(512), self.public, "test-key",
                               max_wasm_bytes=1024)

    def test_ustar_length_type_checksum_and_padding_rejected(self) -> None:
        manifest, _, package = self._package()
        signature_header, wasm_header, _ = self._member_offsets(manifest)
        changed = self._header_field(package, wasm_header, 124, 136,
                                     f"{1025:011o}\0".encode("ascii"))
        with self.assertRaisesRegex(pkg.PackageError, "长度超限"):
            pkg.unpack(changed, max_wasm_bytes=1024)
        changed = self._header_field(package, signature_header, 156, 157, b"2")
        with self.assertRaisesRegex(pkg.PackageError, "类型"):
            pkg.unpack(changed, max_wasm_bytes=1024)
        changed = bytearray(package)
        changed[148] = ord("7") if changed[148] != ord("7") else ord("6")
        with self.assertRaisesRegex(pkg.PackageError, "校验和"):
            pkg.unpack(bytes(changed), max_wasm_bytes=1024)
        changed = bytearray(package)
        changed[512 + len(manifest)] = 1
        with self.assertRaisesRegex(pkg.PackageError, "填充非零"):
            pkg.unpack(bytes(changed), max_wasm_bytes=1024)

    def test_noncanonical_ustar_header_rejected_with_valid_checksum(self) -> None:
        _, _, package = self._package()
        changed = bytearray(package)
        changed[500] = 1
        changed[148:156] = b"        "
        checksum = sum(changed[:512])
        changed[148:156] = f"{checksum:06o}\0 ".encode("ascii")
        with self.assertRaisesRegex(pkg.PackageError, "非固定规范"):
            pkg.unpack(bytes(changed), max_wasm_bytes=1024)

    def test_package_limit_rejected(self) -> None:
        _, _, package = self._package()
        with self.assertRaises(pkg.PackageError):
            pkg.unpack(package, max_wasm_bytes=7)
        oversized = WASM + section(0, name("name") + bytes(pkg.DEVICE_MAX_WASM_BYTES))
        with self.assertRaisesRegex(pkg.PackageError, "设备扫描上限"):
            pkg.create_manifest(SPEC, oversized)


if __name__ == "__main__":
    unittest.main()
