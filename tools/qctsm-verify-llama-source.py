#!/usr/bin/env python3
"""Read-only byte validation of the locked upstream llama.cpp source tree."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys


def verify(source: Path) -> None:
    metadata = source.lstat()
    if (not stat.S_ISDIR(metadata.st_mode) or source.is_symlink()
            or getattr(metadata, "st_file_attributes", 0) & 0x400):
        raise ValueError(f"The llama.cpp source must be an existing directory, not a symlink: {source}")
    project = Path(__file__).resolve().parents[1]
    lock = json.loads((project / "dependencies.lock.json").read_text(encoding="utf-8"))["llama.cpp"]
    manifest = (project / lock["sourceManifest"]).read_bytes()
    if hashlib.sha256(manifest).hexdigest() != lock["sourceManifestSha256"]:
        raise ValueError("The llama.cpp source manifest differs from its reviewed hash")
    expected: dict[str, str] = {}
    for entry in manifest.decode("utf-8").splitlines():
        digest, separator, relative = entry.partition("  ")
        path = PurePosixPath(relative)
        if (separator != "  " or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest)
                or not relative or path.is_absolute() or ".." in path.parts or relative in expected):
            raise ValueError("Invalid llama.cpp source manifest entry")
        expected[relative] = digest

    actual: dict[str, Path] = {}
    pending = [source]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as children:
            for entry in children:
                if directory == source and entry.name == ".git":
                    continue
                path = Path(entry.path)
                relative = path.relative_to(source).as_posix()
                metadata = entry.stat(follow_symlinks=False)
                # Windows junctions also carry this reparse-point attribute.
                if entry.is_symlink() or getattr(metadata, "st_file_attributes", 0) & 0x400:
                    raise ValueError(f"The llama.cpp source contains a symlink: {relative}")
                if stat.S_ISDIR(metadata.st_mode):
                    pending.append(path)
                elif stat.S_ISREG(metadata.st_mode):
                    actual[relative] = path
                else:
                    raise ValueError(f"The llama.cpp source contains a non-regular file: {relative}")
    if actual.keys() != expected.keys():
        raise ValueError(f"The llama.cpp source file set differs from the locked {lock['tag']} archive")
    for relative, path in actual.items():
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
        if digest.hexdigest() != expected[relative]:
            raise ValueError(f"The llama.cpp source file differs from its locked hash: {relative}. "
                             "Use pristine upstream bytes with Git core.autocrlf=false.")


if __name__ == "__main__":
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        verify(arguments.source)
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
