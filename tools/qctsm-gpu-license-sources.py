#!/usr/bin/env python3
"""Pinned ROCm notice collection, adapted from QCopilots-Search (MIT, xbyang)."""
from __future__ import annotations
import hashlib, json, os, re, tarfile, tempfile, urllib.request, zipfile
from pathlib import Path, PurePosixPath
SOURCE = Path(__file__).resolve().parent.parent
GPU_LICENSE_LOCK = SOURCE / "config/gpu-license-sources.lock.json"
MAX_FILE = 16 * 1024 * 1024

def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result

def read_json(path):
    path = Path(path)
    if path.stat().st_size > MAX_FILE:
        raise ValueError(f"License JSON exceeds size limit: {path}")
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)

def relative_path(name):
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe license path: {name}")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts) or PurePosixPath(name).is_absolute():
        raise ValueError(f"Unsafe license path: {name}")
    return PurePosixPath(name)

def safe_file(root, name):
    path = Path(root).joinpath(*relative_path(name).parts)
    if not path.resolve().is_relative_to(Path(root).resolve()):
        raise ValueError(f"License path escapes its root: {name}")
    for item in (path, *path.parents):
        if item == Path(root):
            break
        if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
            raise ValueError(f"Linked license material: {name}")
    return path

def fetch_gpu_license_sources(backend, version, cache, download=False):
    """Fetch pinned ROCm redistribution originals missing from its binary wheels."""
    if backend not in {"hip", "rocm"}:
        return {"schemaVersion": 1, "resources": []}
    lock = read_json(GPU_LICENSE_LOCK)
    if lock.get("schemaVersion") != 1 or version not in lock.get("rocm", {}):
        raise ValueError(f"No reviewed runtime license sources for {backend} {version}")
    selection = lock["rocm"][version]
    cache = Path(cache).expanduser().resolve()
    if cache == SOURCE or cache.is_relative_to(SOURCE) or cache == Path(cache.anchor):
        raise ValueError("GPU license cache must be outside the checkout and filesystem root")
    cache.mkdir(parents=True, exist_ok=True)

    def fetch(entry, maximum):
        expected, size, url = entry.get("sha256", ""), entry.get("size"), entry.get("url", "")
        if (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)
                or not isinstance(size, int) or not 0 < size <= maximum
                or not isinstance(url, str) or not url.startswith("https://")):
            raise ValueError("Invalid pinned GPU license source record")
        destination = safe_file(cache, expected)
        if destination.exists():
            if not destination.is_file() or destination.stat().st_size != size or digest(destination) != expected:
                raise ValueError(f"Cached GPU license source SHA-256/size differs: {url}")
            return destination
        if not download:
            raise ValueError(f"Verified GPU license source is missing: {url}")
        descriptor, filename = tempfile.mkstemp(prefix="qctsm-gpu-license-", suffix=".part", dir=cache)
        temporary = Path(filename)
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "qcopilots-tool-semantic-matching/license-materials"})
            with os.fdopen(descriptor, "wb") as output, urllib.request.urlopen(request, timeout=120) as response:
                received = 0
                for block in iter(lambda: response.read(1024 * 1024), b""):
                    received += len(block)
                    if received > size:
                        raise ValueError(f"GPU license source exceeds locked size: {url}")
                    output.write(block)
            if temporary.stat().st_size != size or digest(temporary) != expected:
                raise ValueError(f"Downloaded GPU license source SHA-256/size differs: {url}")
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)
        return destination

    archives = {entry["name"]: entry for entry in selection.get("sourceArchives", [])}
    resources = []
    for resource in selection["resources"]:
        files = []
        for entry in resource["files"]:
            relative_path(entry["path"])
            if "archive" in entry:
                if entry["archive"] not in archives:
                    raise ValueError(f"GPU license archive is not locked: {entry['archive']}")
                archive_path = fetch(archives[entry["archive"]], 128 * 1024 * 1024)
                member = relative_path(entry["member"]).as_posix()
                size, expected = entry.get("size"), entry.get("sha256", "")
                if (not isinstance(size, int) or not 0 < size <= MAX_FILE
                        or not re.fullmatch(r"[0-9a-f]{64}", expected)):
                    raise ValueError("Invalid GPU license archive member lock")
                if zipfile.is_zipfile(archive_path):
                    with zipfile.ZipFile(archive_path) as archive:
                        matches = [info for info in archive.infolist() if info.filename == member]
                        if (len(matches) != 1 or matches[0].file_size != size
                                or (matches[0].external_attr >> 16) & 0o170000 == 0o120000):
                            raise ValueError(f"Invalid GPU license ZIP member: {member}")
                        data = archive.read(matches[0])
                else:
                    with tarfile.open(archive_path, "r:*") as archive:
                        matches = [info for info in archive.getmembers() if info.name == member]
                        if len(matches) != 1 or not matches[0].isfile() or matches[0].size != size:
                            raise ValueError(f"Invalid GPU license tar member: {member}")
                        data = archive.extractfile(matches[0]).read(MAX_FILE + 1)
                if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
                    raise ValueError(f"GPU license archive member SHA-256 differs: {member}")
                source = safe_file(cache, expected)
                if source.exists() and source.read_bytes() != data:
                    raise ValueError(f"Cached GPU license original differs: {member}")
                if not source.exists():
                    source.write_bytes(data)
            else:
                source = fetch(entry, MAX_FILE)
            files.append({"path": entry["path"], "source": str(source), "sha256": entry["sha256"]})
        resources.append({"name": resource["name"], "version": resource["version"], "files": files})
    return {"schemaVersion": 1, "resources": resources}
