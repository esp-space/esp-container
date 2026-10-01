#!/usr/bin/env python3
"""从无探针签名构建的 ELF/map 生成可审计的紧凑收据。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ARCHIVES = {
    "frp": ("esp_frp", "esp_frp"),
    "mqtt": ("mqtt", "mqtt"),
    "ota": ("esp_ota", "esp_ota"),
    "container": ("esp_container", "esp_container"),
    "binding": ("container_binding", "container_binding"),
    "wamr": ("wasm-micro-runtime", "wasm-micro-runtime"),
}
SYMBOLS = (
    "efrp_tls_step",
    "emqtt_create",
    "esp_mqtt_client_start",
    "eota_preflight",
    "esp_base_container_reconcile",
    "econtainer_runtime_open",
    "econtainer_slots_reconcile",
    "wasm_interp_call_wasm",
)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def audit(root: Path, target: str) -> dict:
    short_name = "c3" if target == "esp32c3" else "esp32"
    build = root / f"build-{short_name}"
    source = root / target
    firmware = source / "firmware"
    binary = build / "esp_base.bin"
    elf = build / "esp_base.elf"
    map_file = build / "esp_base.map"
    build_log = (root / f"{short_name}-build.log").read_text()
    verify_log = (root / f"{short_name}-verify.log").read_text()
    nm = (root / f"{short_name}-nm.txt").read_text()
    map_text = map_file.read_text()
    size_line = re.search(r"esp_base\.bin binary size .+", build_log)
    bootloader_line = re.search(r"Bootloader binary size .+", build_log)
    if not size_line or not bootloader_line or "Project build complete" not in build_log:
        raise SystemExit(f"{target} 官方构建尺寸收据不完整")
    expected_verify = (
        "Signature block 0 verification successful" if target == "esp32c3"
        else "Signature is valid."
    )
    if expected_verify not in verify_log:
        raise SystemExit(f"{target} 官方签名验证失败")
    if any((firmware / "apps/esp_base/main" / name).exists() for name in
           ("capacity_references.c", "capacity_runtime_probe.c", "runtime_guest_bytes.h")):
        raise SystemExit(f"{target} 主应用出现容量 fixture")
    retained = set()
    for line in nm.splitlines():
        match = re.fullmatch(r"[0-9a-fA-F]+\s+[A-Za-z]\s+(\S+)", line)
        if match:
            retained.add(match.group(1))
    libraries = {}
    for label, (component, library) in ARCHIVES.items():
        pattern = re.compile(rf"esp-idf/{re.escape(component)}/lib{re.escape(library)}\.a\(([^)]+)\)")
        members = sorted(set(pattern.findall(map_text)))
        archive = build / "esp-idf" / component / f"lib{library}.a"
        libraries[label] = {
            "compiled_archive_bytes": archive.stat().st_size,
            "map_member_count": len(members),
            "map_members": members,
        }
    return {
        "target": target,
        "signed_image_bytes": binary.stat().st_size,
        "signed_image_sha256": digest(binary),
        "elf_sha256": digest(elf),
        "map_sha256": digest(map_file),
        "sdkconfig_sha256": digest(root / f"{short_name}-sdkconfig"),
        "dependencies_lock_sha256": digest(firmware / ("dependencies.lock" if target == "esp32c3" else "dependencies.lock.esp32")),
        "partition_binary_sha256": digest(build / "partition_table/partition-table.bin"),
        "base_main_source_sha256": digest(firmware / "apps/esp_base/main/esp_base_main.c"),
        "base_main_cmake_sha256": digest(firmware / "apps/esp_base/main/CMakeLists.txt"),
        "official_size_line": size_line.group(0),
        "official_bootloader_line": bootloader_line.group(0),
        "official_signature_valid": True,
        "retained_symbols": {name: name in retained for name in SYMBOLS},
        "libraries": libraries,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--idf", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    bundle = args.bundle.resolve()
    idf = args.idf.resolve()
    source_manifest = json.loads((root / "source-manifest.json").read_text())
    for name, expected in source_manifest["archives"].items():
        if digest(bundle / f"{name}.tar") != expected:
            raise SystemExit(f"{name} 输入归档已变化")
    result = {
        "source_manifest": source_manifest,
        "idf_commit": subprocess.check_output(
            ["git", "-C", str(idf), "rev-parse", "HEAD"], text=True
        ).strip(),
        "lwip_commit": subprocess.check_output(
            ["git", "-C", str(idf / "components/lwip/lwip"), "rev-parse", "HEAD"],
            text=True,
        ).strip(),
        "wamr_component_hash": (root / "esp32c3/firmware/managed_components/wasm-micro-runtime/.component_hash").read_text().strip(),
        "builds": [audit(root, target) for target in ("esp32c3", "esp32")],
    }
    print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
