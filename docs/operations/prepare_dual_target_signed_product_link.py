#!/usr/bin/env python3
"""以精确 Git 归档准备无探针的双目标签名 Base + 可选 Container 编译。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


COMMITS = {
    "base": "7ec2738d678735a7f8e2965325ceac2d3f98c9f8",
    "frp": "1f0c8f37db3765a74b3b95871bb266d0c73d1248",
    "mqtt": "9d6d95e779f4f5ff387a6d9b54015bf4e43565f2",
    "ota": "207273188b984161362824c3344614e812016836",
    "container": "8eb805f3f12cb3cd836e9833acb4aca878ae80e7",
}
MANAGED_HASHES = {
    "espressif__cjson": "e788323270d90738662d66fffa910bfe1fba019bba087f01557e70c40485b469",
    "wasm-micro-runtime": "a799be27248cadffdee6f6fae988dbdf3f5d423baab0b1008d74b8581b5ed507",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def export(args: argparse.Namespace) -> None:
    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f"拒绝覆盖输入包：{output}")
    output.mkdir(parents=True)
    manifest = {"commits": COMMITS, "archives": {}}
    for name, commit in COMMITS.items():
        repo = getattr(args, name).resolve()
        actual = subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", f"{commit}^{{commit}}"], text=True
        ).strip()
        if actual != commit:
            raise SystemExit(f"{name} 提交与预期不符：{actual}")
        archive = output / f"{name}.tar"
        with archive.open("wb") as target:
            subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", commit],
                           stdout=target, check=True)
        manifest["archives"][name] = sha256(archive)
    (output / "source-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    )


def extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True)
    with tarfile.open(archive) as source:
        for item in source.getmembers():
            if item.name.startswith("/") or ".." in Path(item.name).parts:
                raise SystemExit(f"非法归档路径：{item.name}")
        source.extractall(destination, filter="data")


def assemble(args: argparse.Namespace) -> None:
    bundle = args.bundle.resolve()
    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f"拒绝覆盖实验目录：{output}")
    manifest = json.loads((bundle / "source-manifest.json").read_text())
    if manifest["commits"] != COMMITS:
        raise SystemExit("源码提交集合不匹配")
    for name in COMMITS:
        archive = bundle / f"{name}.tar"
        if sha256(archive) != manifest["archives"][name]:
            raise SystemExit(f"{name} 归档摘要不匹配")
    cache = args.managed_cache.resolve()
    for name, expected_hash in MANAGED_HASHES.items():
        component = cache / name
        if (component / ".component_hash").read_text().strip() != expected_hash:
            raise SystemExit(f"受锁组件缓存摘要不匹配：{component}")
    keys = {"esp32c3": args.c3_key.resolve(), "esp32": args.esp32_key.resolve()}
    for key in keys.values():
        if not key.is_file() or key.is_relative_to(output):
            raise SystemExit("测试键必须位于仓外、实验工程外且已存在")

    output.mkdir(parents=True)
    sources = output / "sources"
    for name in COMMITS:
        extract(bundle / f"{name}.tar", sources / name)
    component_origins = {
        "esp_frp": sources / "frp",
        "mqtt": sources / "mqtt",
        "esp_ota": sources / "ota/components/esp_ota",
        "esp_container": sources / "container/components/esp_container",
    }
    for target in ("esp32c3", "esp32"):
        candidate = output / target
        shutil.copytree(sources / "base", candidate)
        firmware = candidate / "firmware"
        for name, origin in component_origins.items():
            shutil.copytree(origin, firmware / "components" / name)
        for name in MANAGED_HASHES:
            shutil.copytree(cache / name, firmware / "managed_components" / name)
        # Signed policy lives only in the isolated test directory. Product
        # defaults, partition tables, application sources and link graph stay exact.
        signed = candidate / "signed.defaults"
        if target == "esp32c3":
            signed.write_text(
                "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n"
                "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y\n"
                "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y\n"
                "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y\n"
                f'CONFIG_SECURE_BOOT_SIGNING_KEY="{keys[target]}"\n'
                "CONFIG_SECURE_BOOT=n\n"
            )
        else:
            signed.write_text(
                "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n"
                "CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME=y\n"
                "CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT=y\n"
                "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y\n"
                "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y\n"
                f'CONFIG_SECURE_BOOT_SIGNING_KEY="{keys[target]}"\n'
                "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n\n"
                "CONFIG_SECURE_BOOT=n\n"
            )
    (output / "source-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    command = parser.add_subparsers(dest="command", required=True)
    export_parser = command.add_parser("export")
    for name in COMMITS:
        export_parser.add_argument(f"--{name}", required=True, type=Path)
    export_parser.add_argument("--output", required=True, type=Path)
    export_parser.set_defaults(func=export)
    assemble_parser = command.add_parser("assemble")
    assemble_parser.add_argument("--bundle", required=True, type=Path)
    assemble_parser.add_argument("--output", required=True, type=Path)
    assemble_parser.add_argument("--managed-cache", required=True, type=Path)
    assemble_parser.add_argument("--c3-key", required=True, type=Path)
    assemble_parser.add_argument("--esp32-key", required=True, type=Path)
    assemble_parser.set_defaults(func=assemble)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
