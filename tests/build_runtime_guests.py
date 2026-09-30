#!/usr/bin/env python3
"""Compile the real counter and narrow runtime fault fixtures with wasi-sdk 33."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import counter_guest  # noqa: E402
import message_counter_guest  # noqa: E402


def two_page_fixture(wasm: bytes) -> bytes:
    """Retain a structurally valid old guest to test the C3 admission cap."""
    reader = counter_guest.Reader(wasm)
    reader.take(8)
    result = bytearray(wasm)
    while reader.offset < len(wasm):
        section = reader.byte()
        size = reader.u32()
        offset = reader.offset
        reader.take(size)
        if section == 5:
            if wasm[offset:offset + size] != b"\x01\x01\x01\x01":
                raise counter_guest.GuestError("counter 内存节不符合单页测试输入")
            result[offset + 2:offset + 4] = b"\x02\x02"
            return bytes(result)
    raise counter_guest.GuestError("counter 缺失内存节")


VARIANTS = {
    "event-read": None,
    "init-loop": "ECONTAINER_INIT_LOOP",
    "event-loop": "ECONTAINER_EVENT_LOOP",
    "stop-loop": "ECONTAINER_STOP_LOOP",
    "stop-fail": "ECONTAINER_STOP_FAIL",
    "wrong-signature": "ECONTAINER_WRONG_SIGNATURE",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wasi-sdk", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    counter_guest.build(args.wasi_sdk, args.output_dir / "counter.wasm")
    counter_guest.build(args.wasi_sdk, args.output_dir / "counter-v2.wasm",
                        ROOT / "examples" / "counter-v2" / "counter.c")
    message_counter_guest.build(args.wasi_sdk, args.output_dir / "message-counter.wasm")
    (args.output_dir / "two-page.wasm").write_bytes(
        two_page_fixture((args.output_dir / "counter.wasm").read_bytes()))
    clang = args.wasi_sdk / "bin" / "clang"
    for name, symbol in VARIANTS.items():
        command = [
            str(clang), "--target=wasm32-unknown-unknown", "-std=c11", "-O2",
            "-Wall", "-Wextra", "-Werror", "-nostdlib", "-ffreestanding",
            "-fno-builtin", "-fno-exceptions", "-fno-stack-protector",
            *(f"-mno-{feature}" for feature in counter_guest.FEATURES_OFF),
        ]
        if symbol is not None:
            command.append(f"-D{symbol}")
        command += [
            "-I", str(ROOT / "guest-sdk" / "include"),
            str(ROOT / "tests" / "runtime_guest.c"),
            str(ROOT / "guest-sdk" / "src" / "econtainer_guest.c"),
            "-Wl,--no-entry",
            "-Wl,--export=econtainer_event_buffer",
            *(f"-Wl,--export={export}" for export in counter_guest.EXPORT_TYPES),
            f"-Wl,--initial-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,--max-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
            f"-Wl,-z,stack-size={counter_guest.STACK_BYTES}",
            "-o", str(args.output_dir / f"{name}.wasm"),
        ]
        subprocess.run(command, check=True)
    host_command = [
        str(clang), "--target=wasm32-unknown-unknown", "-std=c11", "-O2",
        "-Wall", "-Wextra", "-Werror", "-nostdlib", "-ffreestanding",
        "-fno-builtin", "-fno-exceptions", "-fno-stack-protector",
        *(f"-mno-{feature}" for feature in counter_guest.FEATURES_OFF),
        "-I", str(ROOT / "guest-sdk" / "include"),
        str(ROOT / "tests" / "host_api_guest.c"),
        str(ROOT / "guest-sdk" / "src" / "econtainer_guest.c"),
        "-Wl,--no-entry", "-Wl,--allow-undefined",
        "-Wl,--export=econtainer_event_buffer",
        *(f"-Wl,--export={export}" for export in counter_guest.EXPORT_TYPES),
        f"-Wl,--initial-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
        f"-Wl,--max-memory={counter_guest.MEMORY_PAGES * counter_guest.PAGE_BYTES}",
        f"-Wl,-z,stack-size={counter_guest.STACK_BYTES}",
        "-o", str(args.output_dir / "host-api.wasm"),
    ]
    subprocess.run(host_command, check=True)
    timer_command = host_command.copy()
    timer_command[timer_command.index(str(ROOT / "tests" / "host_api_guest.c"))] = str(
        ROOT / "tests" / "timer_guest.c"
    )
    timer_command[-1] = str(args.output_dir / "timer.wasm")
    subprocess.run(timer_command, check=True)
    deadline_command = host_command.copy()
    deadline_command[deadline_command.index(str(ROOT / "tests" / "host_api_guest.c"))] = str(
        ROOT / "tests" / "deadline_guest.c"
    )
    deadline_command[-1] = str(args.output_dir / "deadline.wasm")
    subprocess.run(deadline_command, check=True)
    memory_command = host_command.copy()
    memory_command[memory_command.index(str(ROOT / "tests" / "host_api_guest.c"))] = str(
        ROOT / "tests" / "memory_guest.c"
    )
    memory_command[-1] = str(args.output_dir / "memory.wasm")
    subprocess.run(memory_command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
