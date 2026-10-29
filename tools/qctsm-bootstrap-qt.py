#!/usr/bin/env python3
"""Install the locked official Qt SDK without writing into the source tree.

Requires py7zr==1.1.3. Qt's versioned 7z URLs and SHA-256 checksums are
reviewed in qt-packages.lock.json. No live metadata determines the install.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def safe_relative(name: str) -> Path:
    value = PurePosixPath(name.replace("\\", "/"))
    if value.is_absolute() or ".." in value.parts or any(":" in p for p in value.parts):
        raise RuntimeError(f"Unsafe archive path: {name}")
    return Path(*value.parts)


def ensure_external(path: Path, source: Path) -> None:
    if path == source or source in path.parents:
        raise RuntimeError(f"SDK and cache must be outside the source tree: {path}")


def download(entry: dict, cache: Path, reserve: int) -> Path:
    expected = entry["sha256"]
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise RuntimeError(f"Invalid locked SHA-256 for {entry['module']}")
    target = cache / (expected[:16] + "-" + Path(entry["url"]).name)
    if target.is_file() and digest(target) == expected:
        print(f"Verified cache: {entry['module']}", flush=True)
        return target
    partial = target.with_name(target.name + ".partial")
    for attempt in range(3):
        try:
            print(f"Downloading {entry['module']}", flush=True)
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "QCopilots-ToolSemanticMatching-bootstrap/1"})
            with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as stream:
                length = int(response.headers.get("Content-Length", "0"))
                if shutil.disk_usage(cache).free < reserve + length:
                    raise RuntimeError("Insufficient disk space for Qt download and free-space reserve")
                hasher = hashlib.sha256()
                for block in iter(lambda: response.read(1024 * 1024), b""):
                    stream.write(block)
                    hasher.update(block)
            if hasher.hexdigest() != expected:
                raise RuntimeError(f"Qt archive SHA-256 mismatch: {entry['module']}")
            if entry.get("size") is not None and partial.stat().st_size != entry["size"]:
                raise RuntimeError(f"Qt archive size mismatch: {entry['module']}")
            partial.replace(target)
            return target
        except Exception:
            partial.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)
    raise AssertionError("Unreachable")


def extract(entry: dict, archive: Path, root: Path, cache: Path, reserve: int) -> None:
    import py7zr
    destination = root / safe_relative(entry["destination"])
    destination.mkdir(parents=True, exist_ok=True)
    resolved_destination = destination.resolve()
    if resolved_destination != root and root not in resolved_destination.parents:
        raise RuntimeError(f"SDK destination escapes installation root: {destination}")
    marker = root / ".qctsm" / (entry["sha256"] + ".json")
    if marker.is_file():
        metadata = json.loads(marker.read_text(encoding="utf-8"))
        if all((destination / safe_relative(name)).exists() for name in metadata["files"]):
            print(f"Already installed: {entry['module']}", flush=True)
            return
    print(f"Extracting {entry['module']}", flush=True)
    # Extract into a new directory, validate all paths, then merge into the kit.
    # py7zr itself also validates archive paths and symlink targets.
    with tempfile.TemporaryDirectory(prefix="qctsm-qt-", dir=cache) as temporary:
        staging = Path(temporary)
        with py7zr.SevenZipFile(archive, "r") as package:
            files = package.list()
            size = sum(item.uncompressed for item in files)
            for item in files:
                safe_relative(item.filename)
            if shutil.disk_usage(cache).free < reserve + size * 2:
                raise RuntimeError(f"Insufficient disk space to extract {entry['module']}")
            if shutil.disk_usage(destination).free < reserve + size:
                raise RuntimeError(f"Insufficient disk space at SDK destination for {entry['module']}")
            package.extractall(path=staging)
        for item in staging.rglob("*"):
            resolved = item.resolve()
            if resolved != staging and staging not in resolved.parents:
                raise RuntimeError(f"Archive link escapes extraction root: {item}")
        shutil.copytree(staging, destination, dirs_exist_ok=True, symlinks=True)
        installed = [str(item.relative_to(staging)).replace("\\", "/") for item in staging.rglob("*") if item.is_file()]
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps({"module": entry["module"], "sha256": entry["sha256"], "files": installed}, indent=2) + "\n", encoding="utf-8")
    print(f"Installed {entry['module']} -> {destination}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", required=True, type=Path)
    parser.add_argument("--cache-dir", required=True, type=Path)
    parser.add_argument("--platform", choices=("windows", "macos", "linux"), default={"Windows": "windows", "Darwin": "macos", "Linux": "linux"}.get(platform.system()))
    parser.add_argument("--lock", type=Path, default=Path(__file__).with_name("qt-packages.lock.json"))
    parser.add_argument("--reserve-gib", type=float, default=3.0)
    parser.add_argument("--download-only", action="store_true")
    args = parser.parse_args()
    if args.reserve_gib < 0:
        parser.error("--reserve-gib must be non-negative")
    if not args.platform:
        parser.error("Unsupported host. Specify --platform for a download-only operation")
    root, cache = args.qt_root.expanduser().resolve(), args.cache_dir.expanduser().resolve()
    source = Path(__file__).resolve().parent.parent
    ensure_external(root, source)
    ensure_external(cache, source)
    cache.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    configuration = lock["platforms"][args.platform]
    entries = configuration["archives"]
    reserve = int(args.reserve_gib * 1024 ** 3)
    # At most two streamed downloads. Extract serially to bound memory usage.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        downloads = [(entry, pool.submit(download, entry, cache, reserve)) for entry in entries]
        for entry, result in downloads:
            archive = result.result()
            if not args.download_only:
                extract(entry, archive, root, cache, reserve)
    if not args.download_only:
        kit = root / safe_relative(configuration["kit"])
        # Relocate qmake and Qt command-line tools to the selected SDK root.
        (kit / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
        qmake = kit / "bin" / ("qmake.exe" if args.platform == "windows" else "qmake")
        result = subprocess.run([str(qmake), "-query", "QT_VERSION"], text=True, capture_output=True, check=True)
        if result.stdout.strip() != lock["qtVersion"]:
            raise RuntimeError(f"Unexpected installed Qt version: {result.stdout.strip()}")
        print(f"Qt {result.stdout.strip()} ready: {kit}", flush=True)
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"qctsm Qt bootstrap: {error}", file=sys.stderr)
        raise SystemExit(1)
