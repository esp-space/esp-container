#!/usr/bin/env python3
"""用固定 wasi-sdk 构建并核对带有界定时的消息计数产品。"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

import counter_guest
import product_package


ROOT = Path(__file__).resolve().parents[1]


def build(sdk: Path, output: Path) -> None:
    if (sdk / "VERSION").read_text(encoding="utf-8").strip() != counter_guest.SDK_VERSION:
        raise counter_guest.GuestError("需要官方 wasi-sdk 33.0+m")
    clang = sdk / "bin/clang"
    version = subprocess.run([str(clang), "--version"], check=True,
                             capture_output=True, text=True).stdout
    if not version.startswith("clang version 22.1.0-wasi-sdk "):
        raise counter_guest.GuestError("WASI SDK clang 版本不匹配")
    with tempfile.TemporaryDirectory() as directory:
        artifact = Path(directory) / "app.wasm"
        command = [
            str(clang), "--target=wasm32-unknown-unknown", "-std=c11", "-O2",
            "-Wall", "-Wextra", "-Werror", "-nostdlib", "-ffreestanding",
            "-fno-builtin", "-fno-exceptions", "-fno-stack-protector",
            *(f"-mno-{feature}" for feature in counter_guest.FEATURES_OFF),
            "-I", str(ROOT / "guest-sdk/include"),
            str(ROOT / "examples/message-counter/message_counter.c"),
            str(ROOT / "guest-sdk/src/econtainer_guest.c"),
            "-Wl,--no-entry", "-Wl,--allow-undefined",
            "-Wl,--export=econtainer_event_buffer",
            *(f"-Wl,--export={name}" for name in counter_guest.EXPORT_TYPES),
            f"-Wl,--initial-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,--max-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,-z,stack-size={counter_guest.STACK_BYTES}",
            "-o", str(artifact),
        ]
        subprocess.run(command, check=True)
        data = artifact.read_bytes()
        spec = json.loads((ROOT / "examples/message-counter/spec.json").read_text())
        # The existing package scanner rejects unresolved imports outside the
        # exact signed capability set; the narrow no-import counter checker
        # intentionally remains unchanged.
        product_package.create_manifest(spec, data)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    print("message counter guest")
    print(f"  output      {output.resolve()}")
    print(f"  size_bytes  {len(data)}")
    print(f"  sha256      {hashlib.sha256(data).hexdigest()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wasi-sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        build(args.wasi_sdk, args.output)
    except (counter_guest.GuestError, product_package.PackageError, OSError,
            subprocess.CalledProcessError) as error:
        parser.exit(1, f"消息计数 guest 构建失败：{error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
