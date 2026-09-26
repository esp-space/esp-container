"""在冻结的 C3 IRAM-off client 探针上派生 OpenETH 连通性切片。"""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys


RUNTIME_SHA256 = "d8e8d8ef83653dd7b5816e23e841581f857b5c53113e65164eab378af7e05c56"
CLIENT_SHA256 = "429e4340848e8465765293970777b35e3d0fad28dbde535548578bf5003d0e68"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(path: Path, before: str, after: str) -> None:
    source = path.read_text()
    if source.count(before) != 1:
        raise ValueError(f"{path}: expected exactly one insertion marker")
    path.write_text(source.replace(before, after, 1))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="frozen IRAM-off signed probe directory")
    parser.add_argument("destination", type=Path, help="new external QEMU directory")
    args = parser.parse_args()
    source = args.source.resolve()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error(f"destination already exists: {destination}")
    subprocess.run([sys.executable,
                    str(Path(__file__).with_name("prepare_c3_frp_client_qemu.py")),
                    "iram-off", str(source), str(destination)], check=True)
    main_dir = destination / "firmware/apps/esp_base/main"
    runtime = main_dir / "capacity_runtime_probe.c"
    if sha256(runtime) != RUNTIME_SHA256 or \
            sha256(main_dir / "frp_client_qemu_probe.c") != CLIENT_SHA256:
        raise ValueError("client probe derivation differs from frozen input")
    (main_dir / "openeth_qemu_probe.c").write_bytes(
        Path(__file__).with_name("openeth_qemu_probe.c").read_bytes())
    cmake = main_dir / "CMakeLists.txt"
    replace_once(cmake, '"frp_client_qemu_probe.c"',
                 '"frp_client_qemu_probe.c" "openeth_qemu_probe.c"')
    replace_once(cmake, 'REQUIRES esp_frp ', 'REQUIRES esp_eth esp_event esp_netif esp_frp ')
    replace_once(runtime,
                 'bool capacity_frp_client_destroy(void);',
                 'bool capacity_frp_client_destroy(void);\n'
                 'bool capacity_openeth_start(void);\n'
                 'void capacity_openeth_stop(void);')
    replace_once(runtime,
                 '                    (void)capacity_frp_client_destroy();\n'
                 '                }',
                 '                    (void)capacity_frp_client_destroy();\n'
                 '                }\n'
                 '                if (capacity_openeth_start()) capacity_openeth_stop();\n'
                 '                else ++s_failures;')
    config = destination / "firmware/sdkconfig"
    replace_once(config, '# CONFIG_ETH_USE_OPENETH is not set',
                 'CONFIG_ETH_USE_OPENETH=y')
    print(f"openeth prepared destination={destination}")
    print(f"openeth_c_sha256={sha256(main_dir / 'openeth_qemu_probe.c')}")
    print(f"runtime_c_sha256={sha256(runtime)}")
    print(f"sdkconfig_sha256={sha256(config)}")


if __name__ == "__main__":
    main()
