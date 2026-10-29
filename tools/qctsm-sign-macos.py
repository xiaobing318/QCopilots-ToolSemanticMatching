#!/usr/bin/env python3
"""Sign the final macOS bundle from its complete code inventory, inside out."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys

THIN = {b"\xfe\xed\xfa\xce": (">", 28), b"\xce\xfa\xed\xfe": ("<", 28),
        b"\xfe\xed\xfa\xcf": (">", 32), b"\xcf\xfa\xed\xfe": ("<", 32)}
FAT = {b"\xca\xfe\xba\xbe": (">", 20), b"\xbe\xba\xfe\xca": ("<", 20),
       b"\xca\xfe\xba\xbf": (">", 32), b"\xbf\xba\xfe\xca": ("<", 32)}
CODE_TYPES = {2, 6, 8}  # MH_EXECUTE, MH_DYLIB, MH_BUNDLE
BUNDLE_SUFFIXES = {".app", ".framework", ".xpc", ".appex", ".bundle", ".plugin"}


def _thin_type(header: bytes) -> int:
    endian, size = THIN[header[:4]]
    if len(header) < size:
        raise ValueError("Truncated Mach-O header")
    return struct.unpack_from(endian + "I", header, 12)[0]


def macho_file_type(path: Path) -> int | None:
    with path.open("rb") as stream:
        header = stream.read(32)
        magic = header[:4]
        if magic in THIN:
            return _thin_type(header)
        if magic not in FAT:
            return None
        endian, width = FAT[magic]
        if len(header) < 8:
            raise ValueError(f"Truncated universal Mach-O header: {path}")
        count = struct.unpack_from(endian + "I", header, 4)[0]
        if (magic == b"\xca\xfe\xba\xbe" and count >= 45 and len(header) >= 11
                and struct.unpack_from(">H", header, 8)[0] > 0
                and header[10] in {1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, 19, 20}):
            return None  # Java major version and a valid first constant-pool tag.
        if count > 64:
            raise ValueError(f"Universal Mach-O architecture count exceeds the supported limit: {path}")
        if count == 0:
            raise ValueError(f"Universal Mach-O has no architecture: {path}")
        stream.seek(8)
        table = stream.read(count * width)
        if len(table) != count * width:
            raise ValueError(f"Truncated universal Mach-O table: {path}")
        size = path.stat().st_size
        ranges = []
        types = set()
        for index in range(count):
            entry = index * width
            offset, length = struct.unpack_from(endian + ("II" if width == 20 else "QQ"), table, entry + 8)
            if offset < 8 + len(table) or length < 28 or offset + length > size:
                raise ValueError(f"Invalid universal Mach-O slice: {path}")
            if any(offset < stop and offset + length > start for start, stop in ranges):
                raise ValueError(f"Overlapping universal Mach-O slices: {path}")
            ranges.append((offset, offset + length))
            stream.seek(offset)
            slice_header = stream.read(min(32, length))
            if slice_header[:4] not in THIN:
                raise ValueError(f"Invalid universal Mach-O slice header: {path}")
            types.add(_thin_type(slice_header))
        if len(types) != 1:
            raise ValueError(f"Universal Mach-O has inconsistent file types: {path}")
        return types.pop()


def _architectures(path: Path) -> list[str]:
    names = {7: "i386", 12: "arm", 0x01000007: "x86_64", 0x0100000C: "arm64"}
    with path.open("rb") as stream:
        header = stream.read(8)
        if header[:4] in THIN:
            values = [struct.unpack_from(THIN[header[:4]][0] + "I", header, 4)[0]]
        else:
            endian, width = FAT[header[:4]]
            count = struct.unpack_from(endian + "I", header, 4)[0]
            table = stream.read(count * width)
            values = [struct.unpack_from(endian + "I", table, index * width)[0] for index in range(count)]
    return [names.get(value, f"cpu-type-0x{value:08x}") for value in values]


def _inside(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _bundle_executable(container: Path) -> Path | None:
    if container.suffix == ".framework":
        name = container.stem
        versions = [item for item in (container / "Versions").glob("*")
                    if item.is_dir() and not item.is_symlink() and (item / name).is_file()]
        if len(versions) > 1:
            raise ValueError(f"Multi-version frameworks require explicit version signing: {container}")
        direct = container / name
        if direct.exists():
            return direct.resolve(strict=True)
        if len(versions) != 1:
            raise ValueError(f"Framework requires one concrete current version: {container}")
        return (versions[0] / name).resolve(strict=True)
    metadata = container / "Contents/Info.plist"
    if not metadata.is_file():
        return None
    with metadata.open("rb") as stream:
        info = plistlib.load(stream)
    name = info.get("CFBundleExecutable")
    if not name:
        return None  # A resource-only bundle has no code-signing unit.
    if not isinstance(name, str) or Path(name).name != name or name in (".", ".."):
        raise ValueError(f"Invalid bundle executable name: {metadata}")
    return (container / "Contents/MacOS" / name).resolve(strict=True)


def plan_bundle(bundle: Path) -> dict:
    bundle = bundle.resolve(strict=True)
    if not bundle.is_dir() or bundle.suffix != ".app":
        raise ValueError("Signing requires an existing .app directory")
    files = {}
    containers = [bundle]
    aliases = []
    for directory, names, filenames in os.walk(bundle, followlinks=False):
        parent = Path(directory)
        for name in names + filenames:
            path = parent / name
            if path.is_symlink():
                target = path.resolve(strict=True)
                if not _inside(target, bundle):
                    raise ValueError(f"Bundle symlink escapes the application: {path}")
                aliases.append(path.relative_to(bundle).as_posix())
            elif path.is_dir() and path.suffix in BUNDLE_SUFFIXES:
                containers.append(path)
        names[:] = [name for name in names if not (parent / name).is_symlink()]
        for name in filenames:
            path = parent / name
            if path.is_symlink():
                continue
            file_type = macho_file_type(path)
            if file_type is not None:
                if file_type not in CODE_TYPES:
                    raise ValueError(f"Unsupported Mach-O object in the application: {path}")
                files[path.resolve()] = file_type
    units = []
    main_executables = set()
    for container in containers:
        executable = _bundle_executable(container)
        if executable is None:
            if container == bundle:
                raise ValueError("The application has no CFBundleExecutable")
            continue
        if not _inside(executable, container) or executable not in files:
            raise ValueError(f"Bundle executable is not an inventoried Mach-O: {container}")
        main_executables.add(executable)
        units.append({"path": container.relative_to(bundle).as_posix(),
                      "executable": executable.relative_to(bundle).as_posix(),
                      "kind": "bundle", "fileType": files[executable]})
    for path, file_type in files.items():
        if path not in main_executables:
            relative = path.relative_to(bundle).as_posix()
            units.append({"path": relative, "executable": relative,
                          "kind": "file", "fileType": file_type})
    units.sort(key=lambda unit: (-len(Path(unit["path"]).parts), unit["path"]))
    if not units or units[-1]["path"] != ".":
        raise ValueError("The outer application must be the last signing unit")
    return {"bundle": str(bundle), "machOFiles": sorted(path.relative_to(bundle).as_posix() for path in files),
            "architectures": {path.relative_to(bundle).as_posix(): _architectures(path) for path in files},
            "aliases": sorted(aliases), "units": units}


def sign_bundle(bundle: Path, identity: str = "-", entitlements: dict | None = None,
                report_path: Path | None = None, runner=subprocess.run) -> dict:
    if not isinstance(identity, str) or not identity.strip():
        raise ValueError("A signing identity is required, use '-' explicitly for ad-hoc signing")
    bundle = bundle.resolve(strict=True)
    if report_path and _inside(report_path.resolve(), bundle):
        raise ValueError("The signature report must be outside the signed application")
    inventory = plan_bundle(bundle)
    report = {"schemaVersion": 1, "identity": identity,
              "signatureType": "ad-hoc" if identity == "-" else "certificate",
              "notarization": "not-assessed", "inventory": inventory,
              "commands": [], "passed": False}
    entitlements = entitlements or {}
    applicable = {unit["executable"]: unit for unit in inventory["units"]}
    for executable, path in entitlements.items():
        if executable not in applicable or applicable[executable]["fileType"] != 2:
            raise ValueError(f"Entitlements require a known executable, not a library: {executable}")
        with Path(path).open("rb") as stream:
            if not isinstance(plistlib.load(stream), dict):
                raise ValueError(f"Entitlements must contain a plist dictionary: {path}")

    def command(*arguments):
        arguments = list(map(str, arguments))
        result = runner(arguments, capture_output=True, text=True, encoding="utf-8",
                        errors="replace", timeout=120, check=False)
        report["commands"].append({"command": arguments, "exitCode": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise RuntimeError(f"Signing command failed ({result.returncode}): {arguments}\n{result.stderr}")

    try:
        # Containment establishes the required signing order. Parallel dylibs
        # may contain dependency cycles but do not contain each other's seals.
        for unit in inventory["units"]:
            options = ["/usr/bin/codesign", "--force", "--sign", identity,
                       "--preserve-metadata=identifier"]
            if identity != "-":
                options.append("--timestamp")
                if unit["fileType"] == 2:
                    options.extend(["--options", "runtime"])
            if unit["executable"] in entitlements:
                options.extend(["--entitlements", str(Path(entitlements[unit["executable"]]).resolve())])
            command(*options, bundle / unit["path"])
        # Verify every concrete file as well as bundle seals. This catches a
        # full-version dylib even when no load command references its filename.
        for relative in inventory["machOFiles"]:
            command("/usr/bin/codesign", "--verify", "--strict", bundle / relative)
        for unit in inventory["units"]:
            if unit["kind"] == "bundle":
                command("/usr/bin/codesign", "--verify", "--strict", bundle / unit["path"])
        command("/usr/bin/codesign", "--verify", "--deep", "--strict", bundle)
        command("/usr/bin/codesign", "--display", "--verbose=4", bundle)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if report_path:
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--identity", default="-")
    parser.add_argument("--entitlements-map", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise RuntimeError("Actual bundle signing requires macOS codesign")
    entitlements = {}
    if args.entitlements_map:
        values = json.loads(args.entitlements_map.read_text(encoding="utf-8"))
        if not isinstance(values, dict):
            raise ValueError("The entitlements map must be a JSON object")
        for executable, value in values.items():
            path = Path(value).expanduser()
            entitlements[executable] = (args.entitlements_map.parent / path).resolve(strict=True)
    result = sign_bundle(args.bundle, args.identity, entitlements, args.report)
    print(json.dumps({"passed": result["passed"], "signatureType": result["signatureType"],
                      "codeFiles": len(result["inventory"]["machOFiles"]), "report": str(args.report)}, indent=2))


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"qctsm signing: {error}", file=sys.stderr)
        raise SystemExit(1)
