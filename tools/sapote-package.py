#!/usr/bin/env python3
"""Build, inspect, and install deterministic Sapote application packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from typing import Any

import fat32_image


MANIFEST_BYTES = 1024
PACKAGE_HEADER_BYTES = 64
PACKAGE_MAGIC = b"SAPOSPK1"
MANIFEST_MAGIC = b"SAPOTEA1"
CAPABILITIES = {
    "console": 1 << 0,
    "system-read": 1 << 1,
    "data-read": 1 << 2,
    "data-write": 1 << 3,
    "time": 1 << 4,
    "entropy": 1 << 5,
    "window": 1 << 6,
    "input": 1 << 7,
    "network": 1 << 8,
    "threads": 1 << 9,
}


class PackageError(ValueError):
    """A named package-format or installation refusal."""


def read_regular(path: Path) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise PackageError(f"not an ordinary input file: {path}")
    return path.read_bytes()


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def text_field(value: Any, width: int, field: str, *, required: bool) -> bytes:
    if not isinstance(value, str) or (required and not value):
        raise PackageError(f"{field} must be a{' non-empty' if required else ''} string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise PackageError(f"{field} must be ASCII") from error
    if len(encoded) >= width or b"\0" in encoded:
        raise PackageError(f"{field} exceeds its {width - 1}-byte bound")
    return encoded + bytes(width - len(encoded))


def identifier(value: Any, field: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 8:
        raise PackageError(f"{field} must contain 1-8 characters")
    upper = value.upper()
    if any(character not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in upper):
        raise PackageError(f"{field} is outside the ASCII 8.3 identifier subset")
    return upper


def short_path(value: Any, field: str, *, required: bool) -> str:
    if value in (None, "") and not required:
        return ""
    if not isinstance(value, str):
        raise PackageError(f"{field} must be a string")
    fat32_image.short_name_bytes(value)
    return value.upper()


def encode_manifest(spec: dict[str, Any], executable: bytes) -> bytes:
    name = text_field(spec.get("name"), 32, "name", required=True)
    app_id = identifier(spec.get("identifier"), "identifier")
    executable_path = short_path(spec.get("executable"), "executable", required=True)
    data_namespace = identifier(spec.get("data_namespace"), "data_namespace")
    resource_directory = spec.get("resource_directory", "")
    if resource_directory:
        resource_directory = identifier(resource_directory, "resource_directory")
    icon = short_path(spec.get("icon", ""), "icon", required=False)
    arguments = spec.get("arguments", [])
    if not isinstance(arguments, list) or len(arguments) > 8:
        raise PackageError("arguments must be a list of at most eight strings")
    memory_limit = spec.get("memory_limit", 16 * 1024 * 1024)
    max_handles = spec.get("max_handles", 64)
    max_threads = spec.get("max_threads", 4)
    if not isinstance(memory_limit, int) or memory_limit < 64 * 1024 \
            or memory_limit > 256 * 1024 * 1024 or memory_limit % 4096:
        raise PackageError("memory_limit must be a page multiple from 64 KiB through 256 MiB")
    if not isinstance(max_handles, int) or not 1 <= max_handles <= 128:
        raise PackageError("max_handles must be from 1 through 128")
    if not isinstance(max_threads, int) or not 1 <= max_threads <= 8:
        raise PackageError("max_threads must be from 1 through 8")
    requested = spec.get("capabilities", [])
    if not isinstance(requested, list) or any(not isinstance(item, str) for item in requested):
        raise PackageError("capabilities must be a list of names")
    unknown = sorted(set(requested) - CAPABILITIES.keys())
    if unknown:
        raise PackageError(f"unknown capabilities: {', '.join(unknown)}")
    capability_bits = 0
    for capability in requested:
        capability_bits |= CAPABILITIES[capability]

    manifest = bytearray(MANIFEST_BYTES)
    manifest[:8] = MANIFEST_MAGIC
    struct.pack_into("<HHIIHHHHQQ", manifest, 8, 1, MANIFEST_BYTES, 1, 0,
                     len(arguments), max_handles, max_threads, 0,
                     capability_bits, memory_limit)
    manifest[64:96] = name
    manifest[96:112] = text_field(app_id, 16, "identifier", required=True)
    manifest[112:128] = text_field(executable_path, 16, "executable", required=True)
    manifest[128:160] = hashlib.sha256(executable).digest()
    manifest[160:176] = text_field(resource_directory, 16, "resource_directory", required=False)
    manifest[176:192] = text_field(data_namespace, 16, "data_namespace", required=True)
    manifest[192:208] = text_field(icon, 16, "icon", required=False)
    for index, argument in enumerate(arguments):
        manifest[208 + index * 32:240 + index * 32] = text_field(
            argument, 32, f"arguments[{index}]", required=True)
    return bytes(manifest)


def decode_text(data: bytes, field: str) -> str:
    try:
        end = data.index(0)
    except ValueError as error:
        raise PackageError(f"{field} is not terminated") from error
    if any(data[end:]):
        raise PackageError(f"{field} has nonzero tail bytes")
    try:
        return data[:end].decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageError(f"{field} is not ASCII") from error


def inspect_manifest(manifest: bytes, executable: bytes) -> dict[str, Any]:
    if len(manifest) != MANIFEST_BYTES or manifest[:8] != MANIFEST_MAGIC:
        raise PackageError("manifest length or magic is invalid")
    (version, size, abi, flags, argument_count, max_handles, max_threads,
     reserved, capabilities, memory_limit) = struct.unpack_from("<HHIIHHHHQQ", manifest, 8)
    if (version, size, abi, flags, reserved) != (1, MANIFEST_BYTES, 1, 0, 0):
        raise PackageError("manifest version, size, flags, or reserved field is invalid")
    if any(manifest[44:64]) or any(manifest[496:]):
        raise PackageError("manifest reserved bytes are nonzero")
    if argument_count > 8 or capabilities & ~sum(CAPABILITIES.values()):
        raise PackageError("manifest argument or capability bits are invalid")
    digest = hashlib.sha256(executable).digest()
    if manifest[128:160] != digest:
        raise PackageError("manifest executable SHA-256 mismatch")
    args = [decode_text(manifest[208 + index * 32:240 + index * 32],
                        f"arguments[{index}]") for index in range(argument_count)]
    if any(any(manifest[208 + index * 32:240 + index * 32])
           for index in range(argument_count, 8)):
        raise PackageError("unused argument records are nonzero")
    names = [name for name, bit in CAPABILITIES.items() if capabilities & bit]
    return {
        "format": version,
        "abi_version": abi,
        "name": decode_text(manifest[64:96], "name"),
        "identifier": decode_text(manifest[96:112], "identifier"),
        "executable": decode_text(manifest[112:128], "executable"),
        "executable_bytes": len(executable),
        "executable_sha256": digest.hex().upper(),
        "memory_limit": memory_limit,
        "max_handles": max_handles,
        "max_threads": max_threads,
        "capabilities": names,
        "resource_directory": decode_text(manifest[160:176], "resource_directory"),
        "data_namespace": decode_text(manifest[176:192], "data_namespace"),
        "icon": decode_text(manifest[192:208], "icon"),
        "arguments": args,
    }


def build_package(spec: dict[str, Any], executable: bytes) -> bytes:
    if not executable or len(executable) > 16 * 1024 * 1024:
        raise PackageError("executable is empty or exceeds the FAT32 file bound")
    manifest = encode_manifest(spec, executable)
    body = manifest + executable
    header = bytearray(PACKAGE_HEADER_BYTES)
    header[:8] = PACKAGE_MAGIC
    struct.pack_into("<HHIQ", header, 8, 1, PACKAGE_HEADER_BYTES,
                     MANIFEST_BYTES, len(executable))
    header[32:64] = hashlib.sha256(body).digest()
    return bytes(header) + body


def parse_package(package: bytes) -> tuple[bytes, bytes, dict[str, Any]]:
    if len(package) < PACKAGE_HEADER_BYTES + MANIFEST_BYTES or package[:8] != PACKAGE_MAGIC:
        raise PackageError("package is truncated or has invalid magic")
    version, header_bytes, manifest_bytes, executable_bytes = struct.unpack_from(
        "<HHIQ", package, 8)
    if (version, header_bytes, manifest_bytes) != (1, PACKAGE_HEADER_BYTES, MANIFEST_BYTES) \
            or any(package[24:32]):
        raise PackageError("package header version, size, or reserved bytes are invalid")
    expected = PACKAGE_HEADER_BYTES + manifest_bytes + executable_bytes
    if expected != len(package):
        raise PackageError("package length does not match its header")
    body = package[PACKAGE_HEADER_BYTES:]
    if hashlib.sha256(body).digest() != package[32:64]:
        raise PackageError("package body SHA-256 mismatch")
    manifest = body[:manifest_bytes]
    executable = body[manifest_bytes:]
    return manifest, executable, inspect_manifest(manifest, executable)


def command_build(args: argparse.Namespace) -> None:
    spec_value = json.loads(read_regular(Path(args.spec)).decode("utf-8"))
    if not isinstance(spec_value, dict):
        raise PackageError("package specification must be one JSON object")
    executable = read_regular(Path(args.executable))
    package = build_package(spec_value, executable)
    atomic_write(Path(args.output), package)
    _, _, report = parse_package(package)
    print(json.dumps({"output": str(Path(args.output)),
                      "package_sha256": hashlib.sha256(package).hexdigest().upper(),
                      **report}, sort_keys=True))


def command_inspect(args: argparse.Namespace) -> None:
    package = read_regular(Path(args.package))
    _, _, report = parse_package(package)
    print(json.dumps({"package": str(Path(args.package)),
                      "package_sha256": hashlib.sha256(package).hexdigest().upper(),
                      **report}, indent=2, sort_keys=True))


def command_install(args: argparse.Namespace) -> None:
    legacy_paths = (args.echo, args.uname, args.cat)
    if any(legacy_paths) and not all(legacy_paths):
        raise PackageError("legacy echo, uname, and cat inputs are all-or-none")
    busyboxes = tuple(read_regular(Path(path)) for path in legacy_paths if path)
    extras: list[tuple[str, bytes]] = []
    identifiers: set[str] = set()
    for path in args.packages:
        manifest, executable, report = parse_package(read_regular(Path(path)))
        app_id = str(report["identifier"])
        if app_id in identifiers:
            raise PackageError(f"duplicate package identifier: {app_id}")
        identifiers.add(app_id)
        extras.append((app_id + ".MAN", manifest))
        extras.append((str(report["executable"]), executable))
    image = fat32_image.build_image("system", busyboxes, tuple(extras))
    fat32_image.verify_system(image, busyboxes, tuple(extras))
    atomic_write(Path(args.output), image)
    print(json.dumps({"output": str(Path(args.output)),
                      "packages": sorted(identifiers),
                      "sha256": hashlib.sha256(image).hexdigest().upper()}, sort_keys=True))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--spec", required=True)
    builder.add_argument("--executable", required=True)
    builder.add_argument("--output", required=True)
    builder.set_defaults(function=command_build)
    inspector = commands.add_parser("inspect")
    inspector.add_argument("package")
    inspector.set_defaults(function=command_inspect)
    installer = commands.add_parser("install-system")
    installer.add_argument("--echo")
    installer.add_argument("--uname")
    installer.add_argument("--cat")
    installer.add_argument("--output", required=True)
    installer.add_argument("packages", nargs="+")
    installer.set_defaults(function=command_install)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
    except (PackageError, fat32_image.Fat32Error, OSError, UnicodeError,
            json.JSONDecodeError, struct.error) as error:
        print(f"Sapote package refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
