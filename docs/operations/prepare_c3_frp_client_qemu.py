"""从已冻结的两种 C3 五仓 QEMU 镜像派生空闲 FRP client 容量实验。"""

import argparse
import hashlib
from pathlib import Path
import shutil


BASELINES = {
    "iram-on": (
        "93fb2b027f5bf8d0acae828e4812cd5665acc2803f21621ea2b7ee169051efdc",
        "cb4911792bf9fc1191e4dfc90ff04e483e880ed6800a455f590e594c5ce6b62d",
    ),
    "iram-off": (
        "4f1935a3898172cb3983600572e6808f11f9a3426783bd393ceeec45a258f059",
        "230e7a60b88a59e98b4b9d4b79aa34155a739fbd2c4c9d32db8218642101909b",
    ),
}
CAPACITY_PROBE_SHA256 = "b1526168e0f0db1a21869a485bd7dc165cdfcbe37e0637ea9d9e0534bf80ea1f"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def replace_once(path: Path, before: str, after: str) -> None:
    original = path.read_text()
    if original.count(before) != 1:
        raise ValueError(f"{path}: expected exactly one insertion marker")
    path.write_text(original.replace(before, after, 1))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=BASELINES)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error(f"destination already exists: {destination}")
    firmware = source / "firmware"
    main_dir = firmware / "apps/esp_base/main"
    expected_app, expected_config = BASELINES[args.profile]
    for path, expected in (
        (firmware / "build/esp_base.bin", expected_app),
        (firmware / "sdkconfig", expected_config),
        (main_dir / "capacity_runtime_probe.c", CAPACITY_PROBE_SHA256),
    ):
        if sha256(path) != expected:
            parser.error(f"source differs from frozen input: {path}")
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("build", "*.log"))
    output_main = destination / "firmware/apps/esp_base/main"
    shutil.copy2(Path(__file__).with_name("frp_client_qemu_probe.c"),
                 output_main / "frp_client_qemu_probe.c")
    replace_once(output_main / "CMakeLists.txt", '"capacity_runtime_probe.c"',
                 '"capacity_runtime_probe.c" "frp_client_qemu_probe.c"')
    runtime = output_main / "capacity_runtime_probe.c"
    replace_once(runtime, 'static const char *const TAG = "five-capacity-auth";',
                 'static const char *const TAG = "five-capacity-auth";\n'
                 'bool capacity_frp_client_create(void);\n'
                 'bool capacity_frp_client_destroy(void);')
    replace_once(runtime,
                 'probe_record("ready_max", aead_65536_bin_start,\n'
                 '                             (size_t)(aead_65536_bin_end - aead_65536_bin_start), 65536, false);',
                 'probe_record("ready_max", aead_65536_bin_start,\n'
                 '                             (size_t)(aead_65536_bin_end - aead_65536_bin_start), 65536, false);\n'
                 '                if (capacity_frp_client_create()) {\n'
                 '                    probe_record("ready_max_with_client", aead_65536_bin_start,\n'
                 '                                 (size_t)(aead_65536_bin_end - aead_65536_bin_start), 65536, false);\n'
                 '                    if (!capacity_frp_client_destroy()) ++s_failures;\n'
                 '                } else {\n'
                 '                    ++s_failures;\n'
                 '                    (void)capacity_frp_client_destroy();\n'
                 '                }')
    print(f"prepared profile={args.profile} source={source} destination={destination}")
    print(f"source_app_sha256={expected_app} source_sdkconfig_sha256={expected_config}")
    print(f"probe_c_sha256={sha256(output_main / 'frp_client_qemu_probe.c')}")
    print(f"runtime_c_sha256={sha256(runtime)}")


if __name__ == "__main__":
    main()
