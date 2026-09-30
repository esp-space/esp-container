from __future__ import annotations

import base64
import copy
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from io import BytesIO
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import product_package as pkg  # noqa: E402
from wasm_fixture import module, name, section  # noqa: E402
from test_product_package import SPEC  # noqa: E402


class BrowserPackageContractTest(unittest.TestCase):
    def test_python_and_browser_agree_on_signed_manifest_wasm_and_archive(self) -> None:
        node = shutil.which("node")
        if not node:
            self.skipTest("需要 Node 22+ 验证 Web Crypto 浏览器 SDK")
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        public = key.public_key().public_bytes(serialization.Encoding.PEM,
                                             serialization.PublicFormat.SubjectPublicKeyInfo)
        cases: list[bytes] = []

        def make(wasm: bytes, changes: dict | None = None, salt: int = 32) -> bytes:
            record = json.loads(pkg.create_manifest(SPEC, module()))
            record["payload"]["size_bytes"] = len(wasm)
            record["payload"]["sha256"] = hashlib.sha256(wasm).hexdigest()
            record.update(changes or {})
            manifest = pkg._json_bytes(record)
            signature = key.sign(pkg.DOMAIN + manifest, padding.PSS(
                mgf=padding.MGF1(hashes.SHA256()), salt_length=salt), hashes.SHA256())
            output = BytesIO()
            with tarfile.open(fileobj=output, mode="w:", format=tarfile.USTAR_FORMAT) as archive:
                for filename, data in zip(pkg.MEMBERS, (manifest, signature, wasm), strict=True):
                    archive.addfile(pkg._member_info(filename, len(data)), BytesIO(data))
            return output.getvalue()

        cases.append(make(module()))
        cases.append(make(module(("monotonic_ms", "log", "timer_start", "timer_cancel")),
                          {"required_capabilities": ["log", "monotonic-time", "timer"]}))
        for wasm in [module(event_type=0), module(memory_flags=0), module(memory_pages=2),
                     module(memory_max=2), module(duplicate_export=True), module(code_count=2),
                     module(buffer_index=1), module(export_buffer=False),
                     module(buffer_global=b"\x7f\x01\x41\x80\x08\x0b"),
                     module(buffer_global=b"\x7f\x00\x41\0\x0b"),
                     module(buffer_global=b"\x7f\x00\x41\x7f\x0b"),
                     module(("gpio",)), module(("monotonic_ms",)),
                     module(extra=section(8, b"\0")),
                     module(extra=section(0, name("target_features") + b"\0")),
                     module(extra=section(1, b"\0"))]:
            cases.append(make(wasm))
        bad_limits = copy.deepcopy(SPEC["limits"])
        bad_limits["stack_limit_bytes"] = True
        for change in [{"product_id": "counter\n"}, {"product_version": "v0.1.0"},
                       {"guest_abi_version": 3}, {"runtime_profile": "aot"},
                       {"data_schema_version": 0}, {"data_schema_version": 0x100000000},
                       {"required_capabilities": ["timer", "log"]}, {"limits": bad_limits},
                       {"signing_key_id": "other"}, {"unknown": 0}]:
            cases.append(make(module(), change))
        cases.append(make(module(), salt=20))
        cases.append(cases[0] + bytes(10240))
        corrupted = bytearray(cases[0])
        corrupted[1058] = 1  # manifest 之后的非零填充。
        cases.append(bytes(corrupted))
        inputs = [{"package": base64.b64encode(case).decode("ascii"),
                   "public_key_pem": public.decode("ascii"), "key_id": "test-key"}
                  for case in cases]
        results = subprocess.run([node, str(ROOT / "tests/browser-package-verify.mjs")],
                                 input=json.dumps(inputs), text=True, capture_output=True,
                                 check=True, timeout=30)
        observed = json.loads(results.stdout)
        self.assertEqual(len(observed), len(cases))
        with tempfile.TemporaryDirectory() as temporary:
            public_path = Path(temporary) / "public.pem"
            public_path.write_bytes(public)
            accepted = 0
            for index, (case, browser) in enumerate(zip(cases, observed, strict=True)):
                try:
                    record = pkg.verify_package(case, public_path, "test-key", max_wasm_bytes=512 * 1024)
                except pkg.PackageError:
                    self.assertFalse(browser["accepted"], f"浏览器接受 Python 拒绝的第 {index} 项")
                else:
                    accepted += 1
                    self.assertTrue(browser["accepted"], f"浏览器拒绝 Python 接受的第 {index} 项")
                    self.assertEqual(browser["manifest"], record)
                    self.assertEqual(browser["package_sha256"], hashlib.sha256(case).hexdigest())
            self.assertEqual(accepted, 2)
