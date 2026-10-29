#!/usr/bin/env python3
"""Check a separately compiled Windows ggml module against its shipped base DLL.

The source lock owns the backend API version. This check catches mismatched
imports and architectures before an installer is produced, without a GPU.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.sections = []
        if self.data[:2] != b"MZ":
            raise ValueError(f"Not a PE image: {path}")
        pe = self.u32(0x3c)
        if self.take(pe, 4) != b"PE\0\0":
            raise ValueError(f"Invalid PE signature: {path}")
        self.machine = self.u16(pe + 4)
        count, optional_size = self.u16(pe + 6), self.u16(pe + 20)
        optional = pe + 24
        magic = self.u16(optional)
        if magic not in (0x10b, 0x20b):
            raise ValueError("Unsupported PE optional header")
        self.width = 8 if magic == 0x20b else 4
        directories = optional + (112 if self.width == 8 else 96)
        if directories + 16 > optional + optional_size:
            raise ValueError("Truncated PE data directories")
        self.export_rva = self.u32(directories)
        self.import_rva = self.u32(directories + 8)
        self.import_size = self.u32(directories + 12)
        self.headers_size = self.u32(optional + 60)
        for index in range(count):
            section = optional + optional_size + index * 40
            virtual_size, rva, raw_size, offset = struct.unpack("<IIII", self.take(section + 8, 16))
            self.sections.append((rva, max(virtual_size, raw_size), offset, raw_size))

    def take(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError(f"Truncated PE structure: {self.path}")
        return self.data[offset:offset + size]

    def u16(self, offset: int) -> int:
        return struct.unpack("<H", self.take(offset, 2))[0]

    def u32(self, offset: int) -> int:
        return struct.unpack("<I", self.take(offset, 4))[0]

    def offset(self, rva: int) -> int:
        if rva < self.headers_size:
            self.take(rva, 1)
            return rva
        for start, size, raw, raw_size in self.sections:
            if start <= rva < start + size:
                if rva - start >= raw_size:
                    raise ValueError("PE RVA points at uninitialized data")
                return raw + rva - start
        raise ValueError(f"Unmapped PE RVA: {rva}")

    def string(self, rva: int) -> str:
        start = self.offset(rva)
        end = self.data.find(b"\0", start, min(start + 4096, len(self.data)))
        if end < 0:
            raise ValueError("Unterminated PE name")
        return self.data[start:end].decode("ascii")

    def exports(self) -> set[str]:
        if not self.export_rva:
            return set()
        table = self.offset(self.export_rva)
        count = self.u32(table + 24)
        if count > 1_000_000:
            raise ValueError("Invalid PE export count")
        names = self.offset(self.u32(table + 32)) if count else 0
        return {self.string(self.u32(names + 4 * index)) for index in range(count)}

    def imports(self) -> dict[str, set[str]]:
        result = {}
        if not self.import_rva:
            return result
        table = self.offset(self.import_rva)
        for index in range(min(self.import_size // 20, 4096)):
            entry = self.take(table + 20 * index, 20)
            if entry == bytes(20):
                return result
            original, _, _, name, first = struct.unpack("<IIIII", entry)
            dll = self.string(name).lower()
            thunk = self.offset(original or first)
            symbols = set()
            for symbol_index in range(1_000_000):
                value = int.from_bytes(self.take(thunk + symbol_index * self.width, self.width), "little")
                if not value:
                    break
                if value & (1 << (self.width * 8 - 1)):
                    symbols.add(f"ordinal:{value & 0xffff}")
                else:
                    symbols.add(self.string(value + 2))
            else:
                raise ValueError("Unterminated PE import thunk")
            result[dll] = symbols
        raise ValueError("Unterminated PE import table")


def verify(module: PE, base: PE) -> dict:
    if module.machine != 0x8664 or base.machine != 0x8664:
        raise ValueError("The Windows x64 GPU variant requires x64 module and ggml-base")
    if "ggml_backend_init" not in module.exports():
        raise ValueError("GPU module does not export ggml_backend_init")
    imports = module.imports()
    required = imports.get(base.path.name.lower())
    if not required:
        raise ValueError(f"GPU module does not import the canonical {base.path.name}")
    missing = sorted(required - base.exports())
    if missing:
        raise ValueError("GPU module imports symbols absent from ggml-base: " + ", ".join(missing))
    return {
        "schemaVersion": 1, "passed": True, "check": "pe-import-export-compatibility",
        "gpuExecutionTested": False,
        "module": {"path": str(module.path), "sha256": hashlib.sha256(module.data).hexdigest()},
        "base": {"path": str(base.path), "sha256": hashlib.sha256(base.data).hexdigest()},
        "imports": {name: sorted(symbols) for name, symbols in sorted(imports.items())},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = verify(PE(args.module), PE(args.base))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"GPU backend ABI check failed: {error}\n")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS Windows GPU module imports match the shipped ggml-base (GPU execution not tested)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
