#!/usr/bin/env python3

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


TRAILER_MAGIC = b"VOSMOD01"
SIGNATURE_SIZE = 64
SIGNING_DOMAIN = b"VeRsitileOS kernel module signature v1\0"
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
KERNEL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = KERNEL_ROOT.parent.resolve()
DEFAULT_PRIVATE_KEY = (
    Path.home() / ".config" / "versitileos" / "module-signing-key.pem"
)
DEFAULT_TRUSTED_KEY = (
    KERNEL_ROOT / "security" / "cert" / "module_signing_public_key.bin"
)


def signing_message(payload):
    digest = hashlib.sha256(payload).digest()
    return SIGNING_DOMAIN + struct.pack("<Q", len(payload)) + digest


def public_key_bytes(private_key):
    with tempfile.TemporaryDirectory() as temporary_directory:
        public_der = Path(temporary_directory) / "public.der"
        subprocess.run(
            [
                "openssl",
                "pkey",
                "-in",
                str(private_key),
                "-pubout",
                "-outform",
                "DER",
                "-out",
                str(public_der),
            ],
            check=True,
        )
        encoded = public_der.read_bytes()

    if not encoded.startswith(ED25519_SPKI_PREFIX):
        raise ValueError("the private key is not an Ed25519 key")
    raw_key = encoded[len(ED25519_SPKI_PREFIX) :]
    if len(raw_key) != 32:
        raise ValueError("the Ed25519 public key has an invalid length")
    return raw_key


def write_public_key(path, raw_key):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(raw_key)
        temporary_path = Path(temporary.name)
    os.chmod(temporary_path, 0o644)
    os.replace(temporary_path, path)


def ensure_private_key_is_external(path):
    try:
        path.resolve().relative_to(REPOSITORY_ROOT)
    except ValueError:
        return
    raise ValueError("private signing keys must be stored outside the repository")


def generate_keypair(private_key, trusted_key, replace_trusted_key):
    ensure_private_key_is_external(private_key)
    if private_key.exists():
        raise FileExistsError(f"refusing to overwrite private key: {private_key}")
    if trusted_key.exists() and not replace_trusted_key:
        raise FileExistsError(f"refusing to overwrite trusted key: {trusted_key}")

    private_key.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=private_key.parent) as temporary_directory:
        temporary_private = Path(temporary_directory) / "module-signing-key.pem"
        subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "ED25519",
                "-out",
                str(temporary_private),
            ],
            check=True,
        )
        os.chmod(temporary_private, 0o600)
        raw_key = public_key_bytes(temporary_private)
        os.replace(temporary_private, private_key)

    write_public_key(trusted_key, raw_key)
    print(f"Private key created: {private_key}")
    print(f"Trusted public key written: {trusted_key}")


def sign_module(private_key, trusted_key, source, destination):
    ensure_private_key_is_external(private_key)
    if source.resolve() == destination.resolve():
        raise ValueError("input and output module paths must differ")

    payload = source.read_bytes()
    trailer_start = len(payload) - len(TRAILER_MAGIC) - SIGNATURE_SIZE
    if trailer_start >= 0 and payload[trailer_start : trailer_start + 8] == TRAILER_MAGIC:
        raise ValueError("input already appears to contain a module signature")
    if public_key_bytes(private_key) != trusted_key.read_bytes():
        raise ValueError("signing key does not match the kernel's trusted public key")

    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary_directory = Path(temporary_directory)
        message_path = temporary_directory / "message.bin"
        signature_path = temporary_directory / "signature.bin"
        message_path.write_bytes(signing_message(payload))
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(private_key),
                "-in",
                str(message_path),
                "-out",
                str(signature_path),
            ],
            check=True,
        )
        signature = signature_path.read_bytes()

    if len(signature) != SIGNATURE_SIZE:
        raise ValueError("OpenSSL returned an invalid Ed25519 signature length")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        temporary.write(payload)
        temporary.write(TRAILER_MAGIC)
        temporary.write(signature)
        temporary_path = Path(temporary.name)
    os.chmod(temporary_path, source.stat().st_mode & 0o777)
    os.replace(temporary_path, destination)


def verify_module(trusted_key, signed_module):
    trusted_public_key = trusted_key.read_bytes()
    signed_data = signed_module.read_bytes()
    trailer_size = len(TRAILER_MAGIC) + SIGNATURE_SIZE
    if len(trusted_public_key) != 32 or len(signed_data) < trailer_size:
        raise ValueError("public key or signed module has an invalid length")

    payload_size = len(signed_data) - trailer_size
    trailer = signed_data[payload_size:]
    if trailer[: len(TRAILER_MAGIC)] != TRAILER_MAGIC:
        raise ValueError("module signature trailer is missing")

    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary_directory = Path(temporary_directory)
        public_der = temporary_directory / "public.der"
        message_path = temporary_directory / "message.bin"
        signature_path = temporary_directory / "signature.bin"
        public_der.write_bytes(ED25519_SPKI_PREFIX + trusted_public_key)
        message_path.write_bytes(signing_message(signed_data[:payload_size]))
        signature_path.write_bytes(trailer[len(TRAILER_MAGIC) :])
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-verify",
                "-pubin",
                "-inkey",
                str(public_der),
                "-keyform",
                "DER",
                "-rawin",
                "-in",
                str(message_path),
                "-sigfile",
                str(signature_path),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Generate module signing keys and sign or verify kernel modules."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    keygen = commands.add_parser("keygen", help="create a local Ed25519 keypair")
    keygen.add_argument("--private-key", type=Path, default=DEFAULT_PRIVATE_KEY)
    keygen.add_argument("--trusted-key", type=Path, default=DEFAULT_TRUSTED_KEY)
    keygen.add_argument(
        "--replace-trusted-key",
        action="store_true",
        help="replace the kernel's public trust key (requires rebuilding the kernel)",
    )

    sign = commands.add_parser("sign", help="append a signature to a module")
    sign.add_argument("--key", type=Path, required=True)
    sign.add_argument("--trusted-key", type=Path, default=DEFAULT_TRUSTED_KEY)
    sign.add_argument("input", type=Path)
    sign.add_argument("output", type=Path)

    verify = commands.add_parser("verify", help="verify a signed module")
    verify.add_argument("--trusted-key", type=Path, default=DEFAULT_TRUSTED_KEY)
    verify.add_argument("module", type=Path)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    if arguments.command == "keygen":
        generate_keypair(
            arguments.private_key,
            arguments.trusted_key,
            arguments.replace_trusted_key,
        )
    elif arguments.command == "sign":
        sign_module(
            arguments.key,
            arguments.trusted_key,
            arguments.input,
            arguments.output,
        )
    else:
        verify_module(arguments.trusted_key, arguments.module)
        print(f"Signature verified: {arguments.module}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"module_signing.py: {error}", file=sys.stderr)
        sys.exit(1)
