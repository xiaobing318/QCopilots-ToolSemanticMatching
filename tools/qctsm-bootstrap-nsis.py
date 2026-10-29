#!/usr/bin/env python3
"""Install the locked portable NSIS compiler in a private external directory.

Uses only the Python standard library. Does not require administrator rights,
run an installer, modify the registry, or change the global PATH.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def archive_path(name: str, prefix: str) -> Path | None:
    path = PurePosixPath(name.replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or any(":" in part for part in path.parts):
        raise RuntimeError(f"Unsafe NSIS archive path: {name}")
    if not path.parts or path.parts[0] != prefix:
        raise RuntimeError(f"Unexpected NSIS archive prefix: {name}")
    return Path(*path.parts[1:]) if len(path.parts) > 1 else None


def ensure_external(path: Path) -> None:
    source = Path(__file__).resolve().parent.parent
    if path == source or source in path.parents:
        raise RuntimeError(f"NSIS tool and cache directories must be outside the source tree: {path}")


def download(lock: dict, cache: Path, reserve: int) -> Path:
    target = cache / f"nsis-{lock['version']}.zip"
    if target.is_file() and target.stat().st_size == lock["size"] and digest(target) == lock["sha256"]:
        print(f"Verified NSIS cache: {target}", flush=True)
        return target
    if shutil.disk_usage(cache).free < reserve + lock["size"]:
        raise RuntimeError("Insufficient disk space to download NSIS and preserve reserve")
    partial = target.with_suffix(".zip.partial")
    for attempt in range(3):
        try:
            print(f"Downloading NSIS {lock['version']}", flush=True)
            request = urllib.request.Request(lock["url"], headers={"User-Agent": "QCopilots-ToolSemanticMatching-bootstrap/1"})
            with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output, 1024 * 1024)
            if partial.stat().st_size != lock["size"] or digest(partial) != lock["sha256"]:
                raise RuntimeError("NSIS archive size or SHA-256 does not match the reviewed lock")
            partial.replace(target)
            return target
        except Exception:
            partial.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)
    raise AssertionError("Unreachable")


def install(lock: dict, archive: Path, destination: Path, cache: Path, reserve: int) -> Path:
    marker = destination / ".qctsm-nsis.json"
    executable = destination / "makensis.exe"
    if marker.is_file() and executable.is_file():
        previous = json.loads(marker.read_text(encoding="utf-8"))
        if previous.get("sha256") == lock["sha256"] and digest(executable) == lock["compilerSha256"] and all((destination / relative).is_file() for relative in previous["files"]):
            print(f"NSIS already installed: {destination}", flush=True)
            return executable
    elif destination.exists() and any(destination.iterdir()):
        raise RuntimeError(f"Refusing to overwrite a nonempty directory without an NSIS ownership marker: {destination}")

    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        size = sum(entry.file_size for entry in package.infolist())
        if shutil.disk_usage(cache).free < reserve + size * 2 or shutil.disk_usage(destination).free < reserve + size:
            raise RuntimeError("Insufficient disk space to extract NSIS and preserve reserve")
        with tempfile.TemporaryDirectory(prefix="qctsm-nsis-", dir=cache) as temporary:
            staging = Path(temporary)
            installed = []
            for entry in package.infolist():
                relative = archive_path(entry.filename, lock["archivePrefix"])
                if relative is None:
                    continue
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise RuntimeError(f"NSIS archive contains unsupported symbolic link: {entry.filename}")
                target = staging / relative
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.open(entry) as source, target.open("wb") as output:
                        shutil.copyfileobj(source, output, 1024 * 1024)
                    installed.append(relative.as_posix())
            if digest(staging / "makensis.exe") != lock["compilerSha256"]:
                raise RuntimeError("Extracted NSIS compiler does not match its locked SHA-256")
            for relative in installed:
                target = destination / relative
                if destination not in target.resolve().parents:
                    raise RuntimeError(f"NSIS destination path escapes private installation root: {target}")
            shutil.copytree(staging, destination, dirs_exist_ok=True)
    marker.write_text(json.dumps({"version": lock["version"], "sha256": lock["sha256"], "files": installed}, indent=2) + "\n", encoding="utf-8")
    return executable


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--cache-dir", required=True, type=Path)
    parser.add_argument("--lock", type=Path, default=Path(__file__).with_name("nsis-package.lock.json"))
    parser.add_argument("--reserve-gib", type=float, default=3.0)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("This private compiler package supports Windows. Use the native NSIS package on other hosts.")
    if args.reserve_gib < 0:
        parser.error("--reserve-gib must be non-negative")
    destination, cache = args.output_dir.expanduser().resolve(), args.cache_dir.expanduser().resolve()
    ensure_external(destination)
    ensure_external(cache)
    if destination == cache or destination in cache.parents or cache in destination.parents:
        parser.error("--output-dir and --cache-dir must be separate directories")
    cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    reserve = int(args.reserve_gib * 1024 ** 3)
    executable = install(lock, download(lock, cache, reserve), destination, cache, reserve)
    result = subprocess.run([str(executable), "/VERSION"], check=True, text=True, capture_output=True)
    if result.stdout.strip() != "v" + lock["version"]:
        raise RuntimeError(f"Unexpected NSIS compiler version: {result.stdout.strip()}")
    print(f"NSIS {lock['version']} ready: {executable}", flush=True)
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"qctsm NSIS bootstrap: {error}", file=sys.stderr)
        raise SystemExit(1)
