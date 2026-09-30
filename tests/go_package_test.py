from __future__ import annotations

import base64
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from browser_package_test import ROOT, pkg, signed_package_cases


class GoPackageContractTest(unittest.TestCase):
    def test_go_and_python_accept_same_independently_signed_bytes(self) -> None:
        go = shutil.which("go")
        if not go:
            self.skipTest("需要 Go 1.25+ 验证 Go 主机 SDK")
        public, cases = signed_package_cases()
        inputs = [{"package": base64.b64encode(case).decode("ascii"),
                   "public_key_pem": public.decode("ascii"), "key_id": "test-key"}
                  for case in cases]
        with tempfile.TemporaryDirectory() as temporary:
            public_path = Path(temporary) / "public.pem"
            public_path.write_bytes(public)
            executable = Path(temporary) / "verify-contract"
            subprocess.run([go, "build", "-o", str(executable), "../tests/go_package_verify.go"],
                           cwd=ROOT / "host", check=True, capture_output=True, timeout=60)
            results = subprocess.run([str(executable)], input=json.dumps(inputs), text=True,
                                     capture_output=True, check=True, timeout=30)
            observed = json.loads(results.stdout)
            self.assertEqual(len(cases), 37)
            self.assertEqual(len(observed), len(cases))
            accepted = 0
            for index, (case, result) in enumerate(zip(cases, observed, strict=True)):
                try:
                    manifest = pkg.verify_package(case, public_path, "test-key", max_wasm_bytes=512 * 1024)
                except pkg.PackageError:
                    self.assertFalse(result["accepted"], f"Go 接受 Python 拒绝的第 {index} 项")
                    self.assertIsNone(result["verified"])
                else:
                    accepted += 1
                    self.assertTrue(result["accepted"], f"Go 拒绝 Python 接受的第 {index} 项")
                    verified = result["verified"]
                    self.assertEqual(verified["manifest"], manifest)
                    self.assertEqual(verified["package_sha256"], hashlib.sha256(case).hexdigest())
                    self.assertEqual(verified["package_size_bytes"], len(case))
                    spki = pkg._public_key(public_path).public_bytes(
                        pkg.serialization.Encoding.DER, pkg.serialization.PublicFormat.SubjectPublicKeyInfo)
                    self.assertEqual(verified["signing_key_fingerprint_sha256"], hashlib.sha256(spki).hexdigest())
            self.assertEqual(accepted, 6)

    def test_standalone_go_fixture_matches_public_source_vector(self) -> None:
        for filename in ("product.pkg", "public-key.pem"):
            self.assertEqual((ROOT / "host/productpkg/testdata" / filename).read_bytes(),
                             (ROOT / "tests/vectors/product-v1" / filename).read_bytes())
