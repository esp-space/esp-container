"""从冻结的 C3 IRAM-off 镜像派生仅供 QEMU 的 FRPS 会话容量实验。"""

import argparse
from datetime import datetime, timezone
import ipaddress
from pathlib import Path
import shutil
import subprocess
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


SIMULATED_TIME = datetime(2026, 9, 27, 12, 0, tzinfo=timezone.utc)
VALID_FROM = datetime(2025, 1, 1, tzinfo=timezone.utc)
VALID_UNTIL = datetime(2030, 1, 1, tzinfo=timezone.utc)


def write_private(path: Path, data: bytes) -> None:
    with path.open("xb") as output:
        output.write(data)
    path.chmod(0o600)


def create_test_certificate(directory: Path) -> None:
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "QEMU FRPS Test CA")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(VALID_FROM).not_valid_after(VALID_UNTIL)
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
          .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                                       key_encipherment=False, data_encipherment=False,
                                       key_agreement=False, key_cert_sign=True, crl_sign=True,
                                       encipher_only=False, decipher_only=False), critical=True)
          .sign(ca_key, hashes.SHA256()))
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    leaf_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "10.0.2.2")])
    leaf = (x509.CertificateBuilder().subject_name(leaf_name).issuer_name(ca_name)
            .public_key(leaf_key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(VALID_FROM).not_valid_after(VALID_UNTIL)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName(
                [x509.IPAddress(ipaddress.IPv4Address("10.0.2.2"))]), critical=False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                                         key_encipherment=False, data_encipherment=False,
                                         key_agreement=False, key_cert_sign=False, crl_sign=False,
                                         encipher_only=False, decipher_only=False), critical=True)
            .sign(ca_key, hashes.SHA256()))
    write_private(directory / "frps_ca.pem", ca.public_bytes(serialization.Encoding.PEM))
    write_private(directory / "frps_server.pem", leaf.public_bytes(serialization.Encoding.PEM))
    write_private(directory / "frps_server_key.pem", leaf_key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))


def replace_once(path: Path, before: str, after: str) -> None:
    source = path.read_text()
    if source.count(before) != 1:
        raise ValueError(f"{path}: expected exactly one insertion marker")
    path.write_text(source.replace(before, after, 1))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="frozen IRAM-off signed probe directory")
    parser.add_argument("destination", type=Path, help="new external QEMU directory")
    parser.add_argument("--port", type=int, required=True, help="unused local TCP port, 1024..65535")
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be 1024..65535")
    source = args.source.resolve()
    destination = args.destination.resolve()
    if destination.exists():
        parser.error(f"destination already exists: {destination}")
    subprocess.run([sys.executable,
                    str(Path(__file__).with_name("prepare_c3_openeth_qemu.py")),
                    str(source), str(destination)], check=True)
    main_dir = destination / "firmware/apps/esp_base/main"
    fixture = destination / "fixture"
    fixture.mkdir(mode=0o700)
    create_test_certificate(fixture)
    shutil.copy2(fixture / "frps_ca.pem", main_dir / "frps_ca.pem")
    (main_dir / "qemu_frps_config.h").write_text(
        "// QEMU-only generated test inputs. Never flash.\n"
        f"#define QEMU_FRPS_PORT {args.port}U\n"
        f"#define QEMU_FRPS_EPOCH {int(SIMULATED_TIME.timestamp())}L\n")
    shutil.copy2(Path(__file__).with_name("frps_session_qemu_probe.c"),
                 main_dir / "frps_session_qemu_probe.c")
    shutil.copy2(Path(__file__).with_name("qemu_frps_fixture.go"),
                 fixture / "qemu_frps_fixture.go")
    cmake = main_dir / "CMakeLists.txt"
    replace_once(cmake, '"openeth_qemu_probe.c"',
                 '"openeth_qemu_probe.c" "frps_session_qemu_probe.c"')
    replace_once(cmake, 'EMBED_FILES "aead_4096.bin" "aead_65536.bin"',
                 'EMBED_FILES "aead_4096.bin" "aead_65536.bin" '
                 'EMBED_TXTFILES "frps_ca.pem"')
    runtime = main_dir / "capacity_runtime_probe.c"
    replace_once(runtime, 'void capacity_openeth_stop(void);',
                 'void capacity_openeth_stop(void);\n'
                 'bool capacity_frps_session_probe(void);')
    replace_once(runtime,
                 'if (capacity_openeth_start()) capacity_openeth_stop();\n'
                 '                else ++s_failures;',
                 'if (capacity_openeth_start()) {\n'
                 '                    if (!capacity_frps_session_probe()) ++s_failures;\n'
                 '                    capacity_openeth_stop();\n'
                 '                } else ++s_failures;')
    print(f"frps session prepared destination={destination} port={args.port}")
    print("test CA/key stay outside Git; no production endpoint or token")


if __name__ == "__main__":
    main()
