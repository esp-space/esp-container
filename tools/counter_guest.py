#!/usr/bin/env python3
"""Build and inspect the deliberately small freestanding counter guest."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SDK_VERSION = """33.0+m
wasi-libc: 161b3195fc25
llvm: 4434dabb6991
llvm-version: 22.1.0
config: f992bcc08219"""
MEMORY_PAGES = 1
PAGE_BYTES = 65536
STACK_BYTES = 4096
EVENT_BUFFER_BYTES = 4096
EXPORT_TYPES = {
    "econtainer_init": ((), (0x7F,)),
    "econtainer_on_event": ((0x7F, 0x7F), (0x7F,)),
    "econtainer_stop": ((), (0x7F,)),
}
SECTIONS = {1, 3, 5, 6, 7, 10}
FEATURES_OFF = (
    "bulk-memory", "bulk-memory-opt", "reference-types", "simd128",
    "relaxed-simd", "tail-call", "atomics", "multivalue", "multimemory",
    "mutable-globals", "nontrapping-fptoint", "sign-ext",
    "call-indirect-overlong", "exception-handling", "extended-const", "gc",
)


class GuestError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def byte(self) -> int:
        if self.offset >= len(self.data):
            raise GuestError("Wasm 结构截断")
        value = self.data[self.offset]
        self.offset += 1
        return value

    def take(self, size: int) -> bytes:
        if size > len(self.data) - self.offset:
            raise GuestError("Wasm 结构截断")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def u32(self) -> int:
        value = 0
        for shift in range(0, 35, 7):
            octet = self.byte()
            if shift == 28 and octet & 0xF0:
                raise GuestError("Wasm 整数溢出")
            value |= (octet & 0x7F) << shift
            if not octet & 0x80:
                return value
        raise GuestError("Wasm 整数溢出")

    def s32(self) -> int:
        value = 0
        for shift in range(0, 35, 7):
            octet = self.byte()
            if shift == 28 and (octet & 0x78) not in (0, 0x78):
                raise GuestError("Wasm 有符号整数溢出")
            value |= (octet & 0x7f) << shift
            if not octet & 0x80:
                if octet & 0x40:
                    value |= -1 << (shift + 7)
                return value
        raise GuestError("Wasm 有符号整数溢出")

    def name(self) -> str:
        try:
            return self.take(self.u32()).decode("utf-8")
        except UnicodeError as exc:
            raise GuestError("Wasm 名称不是 UTF-8") from exc

    def end(self) -> None:
        if self.offset != len(self.data):
            raise GuestError("Wasm section 有尾随数据")


def _types(data: bytes) -> list[tuple[tuple[int, ...], tuple[int, ...]]]:
    reader = Reader(data)
    result = []
    for _ in range(reader.u32()):
        if reader.byte() != 0x60:
            raise GuestError("只允许函数类型")
        params = tuple(reader.take(reader.u32()))
        returns = tuple(reader.take(reader.u32()))
        result.append((params, returns))
    reader.end()
    if set(result) != set(EXPORT_TYPES.values()) or len(result) != 2:
        raise GuestError("counter 函数类型与 guest ABI 不符")
    return result


def _functions(data: bytes, types: list[tuple[tuple[int, ...], tuple[int, ...]]]) -> list[tuple[tuple[int, ...], tuple[int, ...]]]:
    reader = Reader(data)
    result = []
    for _ in range(reader.u32()):
        index = reader.u32()
        if index >= len(types):
            raise GuestError("函数类型索引越界")
        result.append(types[index])
    reader.end()
    if len(result) != 3:
        raise GuestError("counter 必须恰好定义三个函数")
    return result


def _memory(data: bytes) -> None:
    reader = Reader(data)
    if (reader.u32(), reader.u32(), reader.u32(), reader.u32()) != (
        1, 1, MEMORY_PAGES, MEMORY_PAGES
    ):
        raise GuestError("counter 内存必须为非共享、固定 64 KiB")
    reader.end()


def _global(data: bytes) -> int:
    reader = Reader(data)
    if (reader.u32(), reader.byte(), reader.byte(), reader.byte()) != (2, 0x7F, 1, 0x41):
        raise GuestError("counter 必须包含栈指针和页内事件区两个 i32 global")
    # The linker supplies a positive i32.const stack top. Its exact address is
    # compiler output, while the reserved stack size is fixed by the link flag.
    stack_top = reader.s32()
    if not STACK_BYTES <= stack_top <= MEMORY_PAGES * PAGE_BYTES or reader.byte() != 0x0B:
        raise GuestError("counter 栈指针超出固定线性内存")
    if (reader.byte(), reader.byte(), reader.byte()) != (0x7F, 0, 0x41):
        raise GuestError("counter 事件区必须由不可变 i32 global 导出")
    buffer_offset = reader.s32()
    buffer_end = buffer_offset + EVENT_BUFFER_BYTES
    if (buffer_offset <= 0 or buffer_end > MEMORY_PAGES * PAGE_BYTES or
            not (buffer_end <= stack_top - STACK_BYTES or buffer_offset >= stack_top) or
            reader.byte() != 0x0B):
        raise GuestError("counter 事件区超出页内静态存储或与栈重叠")
    reader.end()
    return 1


def _exports(data: bytes, functions: list[tuple[tuple[int, ...], tuple[int, ...]]],
             buffer_global: int) -> None:
    reader = Reader(data)
    result: dict[str, tuple[int, int]] = {}
    for _ in range(reader.u32()):
        name = reader.name()
        if name in result:
            raise GuestError("Wasm 导出名称重复")
        result[name] = (reader.byte(), reader.u32())
    reader.end()
    if (set(result) != set(EXPORT_TYPES) | {"memory", "econtainer_event_buffer"} or
            result["memory"] != (2, 0) or
            result["econtainer_event_buffer"] != (3, buffer_global)):
        raise GuestError("counter 导出集合与 guest ABI 不符")
    function_indexes = []
    for name, expected_type in EXPORT_TYPES.items():
        kind, index = result[name]
        if kind != 0 or index >= len(functions) or functions[index] != expected_type:
            raise GuestError(f"{name} 的 Wasm 函数签名不符")
        function_indexes.append(index)
    if len(set(function_indexes)) != 3:
        raise GuestError("counter 入口不能复用同一个函数")


def _code(data: bytes, function_count: int) -> None:
    reader = Reader(data)
    if reader.u32() != function_count:
        raise GuestError("Wasm 函数与代码数量不符")
    for _ in range(function_count):
        body = reader.take(reader.u32())
        if len(body) < 2 or body[-1] != 0x0B:
            raise GuestError("Wasm 函数体缺失结束指令")
    reader.end()


def check_wasm(data: bytes) -> None:
    """Check the sample's static ABI/profile; WAMR still validates instructions."""
    reader = Reader(data)
    if reader.take(8) != b"\x00asm\x01\x00\x00\x00":
        raise GuestError("不是标准 Wasm v1 模块")
    sections: dict[int, bytes] = {}
    last_section = 0
    while reader.offset < len(data):
        section = reader.byte()
        content = reader.take(reader.u32())
        if section == 0:
            custom = Reader(content).name()
            if custom not in {"name", "producers"}:
                raise GuestError(f"不允许的自定义节或目标特性：{custom}")
            continue
        if section not in SECTIONS or section <= last_section:
            raise GuestError("Wasm section 不属于 counter Classic profile 或顺序错误")
        sections[section] = content
        last_section = section
    if set(sections) != SECTIONS:
        raise GuestError("counter 必需的 Wasm section 不完整")
    types = _types(sections[1])
    functions = _functions(sections[3], types)
    _memory(sections[5])
    buffer_global = _global(sections[6])
    _exports(sections[7], functions, buffer_global)
    _code(sections[10], len(functions))


def build(sdk: Path, output: Path, source: Path | None = None) -> None:
    if (sdk / "VERSION").read_text(encoding="utf-8").strip() != SDK_VERSION:
        raise GuestError("WASI SDK 版本/源码锁不匹配：需要官方 wasi-sdk-33")
    clang = sdk / "bin" / "clang"
    version = subprocess.run([str(clang), "--version"], check=True, capture_output=True,
                             text=True).stdout
    if not version.startswith("clang version 22.1.0-wasi-sdk "):
        raise GuestError("WASI SDK clang 版本不匹配")
    with tempfile.TemporaryDirectory() as temporary:
        artifact = Path(temporary) / "app.wasm"
        command = [
            str(clang), "--target=wasm32-unknown-unknown", "-std=c11", "-O2",
            "-Wall", "-Wextra", "-Werror", "-nostdlib", "-ffreestanding",
            "-fno-builtin", "-fno-exceptions", "-fno-stack-protector",
            *(f"-mno-{feature}" for feature in FEATURES_OFF),
            "-I", str(ROOT / "guest-sdk" / "include"),
            str(source if source is not None else ROOT / "examples" / "counter" / "counter.c"),
            str(ROOT / "guest-sdk" / "src" / "econtainer_guest.c"),
            "-Wl,--no-entry",
            "-Wl,--export=econtainer_event_buffer",
            *(f"-Wl,--export={name}" for name in EXPORT_TYPES),
            f"-Wl,--initial-memory={MEMORY_PAGES * PAGE_BYTES}",
            f"-Wl,--max-memory={MEMORY_PAGES * PAGE_BYTES}",
            f"-Wl,-z,stack-size={STACK_BYTES}",
            "-o", str(artifact),
        ]
        subprocess.run(command, check=True)
        data = artifact.read_bytes()
        check_wasm(data)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    print(f"counter guest: {output.resolve()}")
    print(f"  wasi-sdk: 33.0+m; memory: {MEMORY_PAGES * PAGE_BYTES} bytes; stack: {STACK_BYTES} bytes")
    print(f"  size_bytes: {len(data)}")
    print(f"  sha256: {hashlib.sha256(data).hexdigest()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build", help="用固定 WASI SDK 构建并检查 counter guest")
    builder.add_argument("--wasi-sdk", required=True, type=Path)
    builder.add_argument("--output", required=True, type=Path)
    builder.add_argument("--source", type=Path,
                         help="guest C 源码；默认使用 examples/counter/counter.c")
    checker = commands.add_parser("check", help="独立检查已有 counter Wasm 的 ABI/profile")
    checker.add_argument("--wasm", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build":
            build(args.wasi_sdk, args.output, args.source)
        else:
            data = args.wasm.read_bytes()
            check_wasm(data)
            print(f"counter guest 检查通过：{args.wasm.resolve()}")
            print(f"  size_bytes: {len(data)}")
            print(f"  sha256: {hashlib.sha256(data).hexdigest()}")
    except (GuestError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"counter guest 检查失败：{exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
