#!/usr/bin/env python3
"""Prepare the selected, versioned GPU SDK outside the source tree.

CUDA downloads have NVIDIA-published SHA256 locks. AMD does not publish whole
wheel checksums in its index: exact URLs, wheel RECORD hashes and the observed
archive SHA256 are recorded explicitly, without claiming a reviewed hash lock.
No model files are downloaded. CPU and Metal require no separate GPU SDK.
"""
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parent.parent
LOCK = SOURCE / "config" / "gpu-sdk.lock.json"
VARIANTS = SOURCE / "config" / "build-variants.json"
RESERVE = 2 * 1024**3


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def safe_relative(value: str) -> Path:
    p = PurePosixPath(value.replace("\\", "/"))
    if not p.parts or p.is_absolute() or ".." in p.parts or any(":" in s for s in p.parts):
        raise RuntimeError(f"Unsafe archive/manifest path: {value!r}")
    return Path(*p.parts)


def external(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path == SOURCE or SOURCE in path.parents:
        raise RuntimeError(f"Generated GPU files must be outside the repository: {path}")
    return path


def write_json(path: Path, value: dict) -> None:
    path = external(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def variant_by_id(identifier: str, path: Path = VARIANTS) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    matches = [v for v in data["variants"] if v["id"] == identifier]
    if len(matches) != 1:
        raise RuntimeError(f"Unknown or duplicate build variant: {identifier}")
    variant = matches[0]
    if variant["backend"] == "hip" and variant["gpuTargets"] != ["gfx1200", "gfx1201"]:
        raise RuntimeError("ROCm packages must contain exactly the reviewed RDNA4 targets gfx1200 and gfx1201")
    return variant


def run(command: list[str], *, capture: bool = False, env: dict | None = None) -> str:
    print("GPU SDK: " + subprocess.list2cmdline([str(x) for x in command]), file=sys.stderr, flush=True)
    result = subprocess.run([str(x) for x in command], check=True, text=True,
                            stdout=subprocess.PIPE if capture else sys.stderr,
                            stderr=subprocess.PIPE if capture else sys.stderr, env=env)
    return result.stdout.strip() if capture else ""


def disk_check(path: Path, additional: int = 0) -> None:
    free = shutil.disk_usage(path).free
    print(f"GPU SDK free space at {path}: {free} bytes", file=sys.stderr)
    if free < RESERVE + additional:
        raise RuntimeError(f"Insufficient disk space at {path}: need {RESERVE + additional}, have {free}")


def download(entry: dict, cache: Path) -> tuple[Path, dict]:
    cache = external(cache)
    cache.mkdir(parents=True, exist_ok=True)
    url = entry["url"]
    if not url.startswith("https://"):
        raise RuntimeError(f"GPU downloads require HTTPS: {url}")
    name = url.rsplit("/", 1)[-1]
    safe_relative(name)
    target = cache / name
    expected = entry.get("sha256")
    if expected is not None and not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise RuntimeError(f"Invalid SHA256 in GPU lock: {name}")
    receipt_path = cache / (name + ".receipt.json")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8")) if receipt_path.exists() else {}
    known_hash = expected or (receipt.get("sha256") if receipt.get("url") == url else None)
    if not (target.is_file() and known_hash and digest(target) == known_hash):
        partial = target.with_name(target.name + ".partial")
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "QCopilots-ToolSemanticMatching-GPU/1"})
            with urllib.request.urlopen(request, timeout=120) as response:
                disk_check(cache, int(response.headers.get("Content-Length", "0")))
                with partial.open("wb") as output:
                    shutil.copyfileobj(response, output, 1024 * 1024)
            observed = digest(partial)
            if expected and observed != expected:
                raise RuntimeError(f"GPU archive SHA256 mismatch: {name}")
            partial.replace(target)
        finally:
            partial.unlink(missing_ok=True)
    observed = digest(target)
    receipt = {"url": url, "sha256": observed, "size": target.stat().st_size,
               "integrity": "publisher-sha256" if expected else "https-versioned-url-observed-sha256"}
    write_json(receipt_path, receipt)
    return target, receipt


def is_license(name: str) -> bool:
    leaf = PurePosixPath(name.replace("\\", "/")).name.lower()
    return bool(re.match(r"^(licen[cs]e|copying|copyright|eula|notice)([._-].*)?$", leaf)
                or re.search(r"[-_](?:licen[cs]e|eula|notice)(?:\.(?:txt|md|rst|html))?$", leaf))


def extract_sdk_archive(archive: Path, destination: Path, component: str) -> list[str]:
    """Merge component payloads, retaining each component's original notices."""
    destination = external(destination)
    destination.mkdir(parents=True, exist_ok=True)
    installed = []
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            disk_check(destination, sum(i.file_size for i in z.infolist()))
            for entry in z.infolist():
                rel = safe_relative(entry.filename)
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise RuntimeError(f"Unexpected symlink in CUDA archive: {entry.filename}")
                if len(rel.parts) < 2 or entry.is_dir():
                    continue
                relative = Path(*rel.parts[1:])
                if is_license(relative.as_posix()):
                    relative = Path(".licenses") / component / relative
                target = destination / relative
                if destination not in target.resolve().parents:
                    raise RuntimeError(f"SDK archive destination escapes root: {target}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(entry) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                installed.append(relative.as_posix())
    else:
        raise RuntimeError(f"Unsupported private SDK archive format: {archive}")
    return installed


def wheel_records(package: zipfile.ZipFile) -> dict[str, tuple[str, int | None]]:
    names = [i.filename for i in package.infolist()]
    if len(set(names)) != len(names):
        raise RuntimeError("Duplicate members in ROCm wheel")
    records = [i.filename for i in package.infolist() if i.filename.endswith(".dist-info/RECORD")]
    if len(records) != 1:
        raise RuntimeError("Expected exactly one wheel RECORD")
    result = {}
    for name, checksum, size in csv.reader(io.StringIO(package.read(records[0]).decode("utf-8"))):
        safe_relative(name)
        if name in result:
            raise RuntimeError(f"Duplicate wheel RECORD member: {name}")
        result[name] = (checksum, int(size) if size else None)
    return result


def copy_wheel_member(package: zipfile.ZipFile, entry: zipfile.ZipInfo,
                      records: dict, output: Path | None) -> None:
    safe_relative(entry.filename)
    if stat.S_ISLNK(entry.external_attr >> 16):
        raise RuntimeError(f"Unexpected wheel symlink: {entry.filename}")
    checksum, expected_size = records.get(entry.filename, ("", None))
    if not checksum.startswith("sha256=") or expected_size != entry.file_size:
        raise RuntimeError(f"Missing SHA256 or wrong size in wheel RECORD: {entry.filename}")
    hasher = hashlib.sha256()
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
    destination = output.open("wb") if output else None
    try:
        with package.open(entry) as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                hasher.update(block)
                if destination:
                    destination.write(block)
    finally:
        if destination:
            destination.close()
    observed = base64.urlsafe_b64encode(hasher.digest()).decode().rstrip("=")
    if observed != checksum.partition("=")[2].rstrip("="):
        if output:
            output.unlink(missing_ok=True)
        raise RuntimeError(f"Wheel RECORD hash mismatch: {entry.filename}")


def is_rocm_device_resource(name: str, targets: list[str]) -> bool:
    path = safe_relative(name).as_posix()
    if not path.startswith("_rocm_sdk_libraries/"):
        return False
    if "/.kpack/blas_lib_" in path:
        return any(path.endswith(f"/blas_lib_{target}.kpack") for target in targets)
    if "/hipblaslt/library/" in path or "/rocblas/library/" in path:
        found = re.findall(r"gfx[0-9a-z]+", path)
        return bool(found) and all(target in targets for target in found)
    return False


def validate_wheel_version(package: zipfile.ZipFile, version: str) -> None:
    metadata = [i for i in package.infolist() if i.filename.endswith(".dist-info/METADATA")]
    if len(metadata) != 1 or f"\nVersion: {version}\n" not in package.read(metadata[0]).decode("utf-8"):
        raise RuntimeError(f"Unexpected ROCm wheel version, expected {version}")


def extract_device_wheel(archive: Path, destination: Path, targets: list[str], version: str = "10.0.0") -> list[dict]:
    destination = external(destination)
    copied = []
    with zipfile.ZipFile(archive) as package:
        validate_wheel_version(package, version)
        records = wheel_records(package)
        selected = [e for e in package.infolist() if not e.is_dir() and
                    (is_rocm_device_resource(e.filename, targets) or is_license(e.filename))]
        if not any(is_rocm_device_resource(e.filename, targets) for e in selected):
            raise RuntimeError(f"ROCm wheel has no BLAS resources for {targets}: {archive.name}")
        disk_check(destination, sum(e.file_size for e in selected))
        for entry in selected:
            target = destination / safe_relative(entry.filename)
            if destination not in target.resolve().parents:
                raise RuntimeError(f"ROCm resource destination escapes root: {target}")
            copy_wheel_member(package, entry, records, target)
            copied.append({"path": target.relative_to(destination).as_posix(), "sha256": digest(target),
                           "size": target.stat().st_size})
    return copied


def verify_wheel(archive: Path, version: str) -> None:
    with zipfile.ZipFile(archive) as package:
        records = wheel_records(package)
        validate_wheel_version(package, version)
        for entry in package.infolist():
            if entry.is_dir() or entry.filename.endswith((".dist-info/RECORD", ".dist-info/RECORD.jws", ".dist-info/RECORD.p7s")):
                continue
            copy_wheel_member(package, entry, records, None)


def cuda_sdk(variant: dict, root: Path, install: bool, lock: dict) -> dict:
    version = variant["toolkitVersion"]
    os_name = variant["os"]
    private = root / ("cuda-" + version)
    compiler_version = lock["cuda"][os_name][version]["compilerVersion"]
    components = lock["cuda"]["windows"][version]["components"] if os_name == "windows" else []

    def component_receipt(component: dict) -> dict | None:
        marker = private / ".qctsm" / (component["name"] + ".json")
        previous = json.loads(marker.read_text(encoding="utf-8")) if marker.exists() else {}
        if previous.get("sha256") == component["sha256"] and previous.get("files") and all(
                (private / safe_relative(name)).is_file() for name in previous["files"]):
            return previous
        return None

    private_complete = bool(components) and all(component_receipt(c) for c in components)
    candidates = [private]
    for name in ("CUDA_PATH", "CUDAToolkit_ROOT"):
        if os.environ.get(name):
            candidates.append(Path(os.environ[name]))
    if os_name == "linux":
        candidates += [Path("/usr/local/cuda")]
    executable = "nvcc.exe" if os_name == "windows" else "nvcc"
    chosen = None
    expected_minor = ".".join(version.split(".")[:2])
    for candidate in candidates:
        if candidate.resolve() == private.resolve() and not private_complete:
            continue
        compiler = candidate / "bin" / executable
        if compiler.is_file():
            output = run([str(compiler), "--version"], capture=True)
            if (re.search(r"release\s+" + re.escape(expected_minor) + r"(?:,|\s)", output)
                    and re.search(r"V" + re.escape(compiler_version) + r"\b", output)):
                chosen = candidate.resolve()
                break
    receipts = []
    if install and os_name == "windows" and (chosen is None or chosen == private.resolve()):
        for component in components:
            previous = component_receipt(component)
            if previous:
                receipts.append(previous)
                continue
            archive, receipt = download(component, root / "downloads")
            marker = private / ".qctsm" / (component["name"] + ".json")
            receipt["files"] = extract_sdk_archive(archive, private, component["name"])
            write_json(marker, receipt)
            receipts.append(receipt)
            archive.unlink()
        chosen = private
        output = run([str(chosen / "bin" / executable), "--version"], capture=True)
        if not (re.search(r"release\s+" + re.escape(expected_minor) + r"(?:,|\s)", output)
                and re.search(r"V" + re.escape(compiler_version) + r"\b", output)):
            raise RuntimeError("Installed CUDA compiler does not match the variant")
    if chosen is None:
        raise RuntimeError(f"CUDA {version} is unavailable. Windows: use --install. Linux: use the locked "
                           f"{variant.get('container', 'CUDA development container')} or set CUDA_PATH to that toolkit.")
    suffix = expected_minor.replace(".", "-")
    licenses = [f"/usr/share/doc/cuda-cudart-{suffix}", f"/usr/share/doc/libcublas-{suffix}"] if os_name == "linux" else []
    return {"environment": {"CUDA_PATH": str(chosen), "PATH": str(chosen / "bin") + os.pathsep + os.environ.get("PATH", "")},
            "cmakeCache": {"CUDAToolkit_ROOT": str(chosen)}, "runtimeRoots": [str(chosen)], "licenseRoots": licenses,
            "requiredLicenseHashes": [lock["cuda"][os_name][version]["runtimeLicenseSha256"]], "downloads": receipts}


def rocm_sdk(variant: dict, root: Path, install: bool, lock: dict) -> dict:
    version = variant["toolkitVersion"]
    private = root / ("rocm-" + version)
    venv = private / "venv"
    windows = variant["os"] == "windows"
    provided_python = os.environ.get("QCOPILOTS_TOOL_SEMANTIC_MATCHING_ROCM_PYTHON")
    python = Path(provided_python).resolve() if provided_python else venv / ("Scripts/python.exe" if windows else "bin/python")
    cli = [str(python), "-m", "rocm_sdk"]
    receipt_file = private / "sdk-downloads.json"
    receipts = json.loads(receipt_file.read_text(encoding="utf-8")) if receipt_file.exists() else {"downloads": []}
    resources = private / "runtime-resources"
    if install:
        private.mkdir(parents=True, exist_ok=True)
        disk_check(private)
        if provided_python and not python.is_file():
            raise RuntimeError(f"QCOPILOTS_TOOL_SEMANTIC_MATCHING_ROCM_PYTHON does not exist: {python}")
        if not python.is_file():
            run([sys.executable, "-m", "venv", str(venv)])
        specs = lock["rocm"][variant["os"]]
        for entry in ([] if provided_python else specs["buildWheels"]):
            marker = private / ".qctsm" / (entry["name"] + ".json")
            if marker.exists():
                continue
            archive, receipt = download(entry, root / "downloads")
            with zipfile.ZipFile(archive) as package:
                disk_check(private, sum(e.file_size for e in package.infolist()))
            verify_wheel(archive, version)
            receipt["integrity"] += "+all-wheel-record-sha256"
            run([str(python), "-m", "pip", "install", "--no-deps", "--no-index", str(archive)])
            write_json(marker, receipt)
            receipts["downloads"].append(receipt)
            archive.unlink()
        selector_marker = private / ".qctsm" / "selector.json"
        if not provided_python and not selector_marker.is_file():
            for entry in lock["rocm"]["buildRequirements"]:
                archive, receipt = download(entry, root / "downloads")
                run([str(python), "-m", "pip", "install", "--no-deps", "--no-index", str(archive)])
                receipts["downloads"].append(receipt)
                archive.unlink()
            archive, receipt = download(lock["rocm"]["selector"], root / "downloads")
            run([str(python), "-m", "pip", "install", "--no-deps", "--no-index", "--no-build-isolation", str(archive)])
            write_json(selector_marker, receipt)
            receipts["downloads"].append(receipt)
            archive.unlink()
        run(cli + ["init"])
        resources.mkdir(parents=True, exist_ok=True)
        for entry in specs["deviceWheels"]:
            marker = private / ".qctsm" / (entry["name"] + ".json")
            previous = json.loads(marker.read_text(encoding="utf-8")) if marker.exists() else {}
            if previous.get("files") and all((resources / safe_relative(f["path"])).is_file() and
                                              digest(resources / safe_relative(f["path"])) == f["sha256"]
                                              for f in previous["files"]):
                continue
            archive, receipt = download(entry, root / "downloads")
            files = extract_device_wheel(archive, resources, variant["gpuTargets"], version)
            receipt["integrity"] += "+selected-wheel-record-sha256"
            write_json(marker, {**receipt, "files": files})
            receipts["downloads"].append(receipt)
            archive.unlink()
        write_json(receipt_file, receipts)
    if not python.is_file():
        raise RuntimeError(f"ROCm {version} SDK is missing. Run bootstrap with --install: {private}")
    versions = json.loads(run([str(python), "-c", "import importlib.metadata as m,json; print(json.dumps({n:m.version(n) for n in ['rocm','rocm-sdk-core','rocm-sdk-libraries','rocm-sdk-devel']}))"], capture=True))
    if any(v != version for v in versions.values()):
        raise RuntimeError(f"Installed ROCm packages do not match {version}: {versions}")
    sdk = Path(run(cli + ["path", "--root"], capture=True)).resolve()
    cmake = run(cli + ["path", "--cmake"], capture=True)
    binary = run(cli + ["path", "--bin"], capture=True)
    site = Path(run([str(python), "-c", "import sysconfig; print(sysconfig.get_paths()['purelib'])"], capture=True))
    for package in ("_rocm_sdk_core", "_rocm_sdk_libraries"):
        if not (site / package).is_dir():
            raise RuntimeError(f"Incomplete ROCm wheel installation: {site / package}")
    for target in variant["gpuTargets"]:
        if not (resources / "_rocm_sdk_libraries" / ".kpack" / f"blas_lib_{target}.kpack").is_file():
            raise RuntimeError(f"ROCm runtime device resources missing for {target}. Run bootstrap --install.")
    compiler = sdk / "lib" / "llvm" / "bin" / ("clang.exe" if windows else "clang")
    if not compiler.is_file():
        raise RuntimeError(f"ROCm HIP compiler is missing after initialization: {compiler}")
    environment = {"HIP_PATH": str(sdk), "ROCM_PATH": str(sdk), "HIP_PLATFORM": "amd",
                   "LLVM_PATH": str(sdk / "lib" / "llvm"), "HIP_DEVICE_LIB_PATH": str(sdk / "lib" / "llvm" / "amdgcn" / "bitcode"),
                   "PATH": binary + os.pathsep + os.environ.get("PATH", "")}
    if not windows:
        environment["LD_LIBRARY_PATH"] = str(sdk / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    return {"environment": environment,
            "cmakeCache": {"QCOPILOTS_TOOL_SEMANTIC_MATCHING_HIP_ROOT": str(sdk), "CMAKE_HIP_COMPILER": str(compiler),
                           "CMAKE_PREFIX_PATH": cmake, "HIP_PLATFORM": "amd"},
            "runtimeRoots": [str(site / "_rocm_sdk_core"), str(site / "_rocm_sdk_libraries")],
            "resourceRoot": str(resources), "downloads": receipts["downloads"],
            "sdkProvenance": "externally-installed-version-verified" if provided_python else "private-verified-wheels"}


def linux_vulkan_sdk(variant: dict, root: Path, install: bool, specification: dict) -> dict:
    """Install reviewed Jammy packages, then inspect the actual system SDK."""
    if variant["toolkitVersion"] != specification["sdkVersion"]:
        raise RuntimeError("Linux Vulkan variant and SDK lock versions differ")
    apt = specification["apt"]
    host = platform.freedesktop_os_release()
    if host.get("ID") != "ubuntu" or host.get("VERSION_ID") != "22.04":
        raise RuntimeError("The locked Linux Vulkan SDK requires Ubuntu 22.04 (Jammy)")
    if platform.machine().lower() not in ("x86_64", "amd64"):
        raise RuntimeError("The locked Linux Vulkan SDK requires an x86_64 host")
    receipts = []
    if install:
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            privileged = []
        else:
            sudo = shutil.which("sudo")
            if not sudo:
                raise RuntimeError("Installing the locked Vulkan SDK requires root or non-interactive sudo")
            privileged = [sudo, "-n", "--"]
        key, receipt = download(apt["signingKey"], root / "downloads")
        receipts.append(receipt)
        keyring = "/usr/share/keyrings/qctsm-lunarg.asc"
        source = external(root / "vulkan-jammy.list")
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(f"deb [arch=amd64 signed-by={keyring}] {apt['repository']} {apt['suite']} {apt['component']}\n",
                          encoding="utf-8", newline="\n")
        run(privileged + ["install", "-D", "-m", "0644", str(key), keyring])
        run(privileged + ["install", "-D", "-m", "0644", str(source), "/etc/apt/sources.list.d/qctsm-lunarg.list"])
        run(privileged + ["env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "update"])
        packages = []
        for package in apt["packages"]:
            archive, receipt = download(package, root / "downloads")
            receipts.append(receipt)
            packages.append(str(archive))
        # All six explicit packages use the reviewed archive bytes. APT handles
        # the SDK metapackage's other dependencies via its signed repositories.
        run(privileged + ["env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "--yes",
                          "--no-install-recommends", *packages])
    installed = []
    for package in apt["packages"]:
        try:
            status = run(["dpkg-query", "--show", "--showformat=${Status}\t${Version}\n", package["name"]], capture=True)
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError(f"Locked Vulkan package {package['name']} is missing. Run bootstrap with --install.") from error
        expected = "install ok installed\t" + package["version"]
        if status != expected:
            raise RuntimeError(f"Vulkan package {package['name']} must be {package['version']}, found {status!r}. Run bootstrap with --install.")
        installed.append({"name": package["name"], "version": package["version"], "archiveSha256": package["sha256"]})
    header = Path("/usr/include/vulkan/vulkan_core.h")
    if not header.is_file() or not re.search(r"#define\s+VK_HEADER_VERSION\s+" + re.escape(specification["sdkVersion"].split(".")[2]) + r"\b", header.read_text(encoding="utf-8")):
        raise RuntimeError("Linux Vulkan headers do not match the locked SDK patch version")
    glslc = Path("/usr/bin/glslc")
    spirv_config = Path("/usr/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake")
    loader = Path("/usr/lib/x86_64-linux-gnu/libvulkan.so")
    if not glslc.is_file() or not spirv_config.is_file() or not loader.is_file():
        raise RuntimeError("Locked Vulkan SDK is incomplete: glslc, the loader or SPIRV-Headers CMake config is missing")
    if digest(glslc) != specification["glslcSha256"]:
        raise RuntimeError("Installed glslc executable differs from the reviewed shaderc package")
    version_output = run([str(glslc), "--version"], capture=True)
    if not version_output.strip():
        raise RuntimeError("The verified glslc executable returned empty version output")
    return {"environment": {}, "cmakeCache": {"Vulkan_GLSLC_EXECUTABLE": str(glslc),
                                              "Vulkan_INCLUDE_DIR": "/usr/include", "Vulkan_LIBRARY": str(loader),
                                              "SPIRV-Headers_DIR": str(spirv_config.parent)},
            "runtimeRoots": ["/usr/lib/x86_64-linux-gnu"], "licenseRoots": ["/usr/share/doc/libvulkan1"],
            "systemPackages": installed, "toolVersions": {"glslc": version_output}, "downloads": receipts}


def vulkan_sdk(variant: dict, root: Path, install: bool, lock: dict) -> dict:
    windows = variant["os"] == "windows"
    if not windows:
        return linux_vulkan_sdk(variant, root, install, lock["vulkan"]["linux"])
    version = variant["toolkitVersion"]
    private = root / ("vulkan-" + version)
    sdk = Path(os.environ.get("VULKAN_SDK", str(private))).resolve()
    compiler = sdk / "Bin" / "glslc.exe"
    receipts = []
    if not compiler.is_file() and install:
        archive, receipt = download(lock["vulkan"]["windows"]["sdk"], root / "downloads")
        run([str(archive), "--root", str(private), "--accept-licenses", "--default-answer", "--confirm-command", "install"])
        sdk, compiler = private, private / "Bin" / "glslc.exe"
        receipts.append(receipt)
        archive.unlink()
    if not compiler.is_file():
        raise RuntimeError(f"Vulkan SDK {version} missing. Run bootstrap --install or set VULKAN_SDK.")
    header = sdk / "Include" / "vulkan" / "vulkan_core.h"
    expected_patch = version.split(".")[2]
    if not header.is_file() or not re.search(r"#define\s+VK_HEADER_VERSION\s+" + re.escape(expected_patch) + r"\b", header.read_text(encoding="utf-8")):
        raise RuntimeError(f"Vulkan SDK headers do not match locked version {version}: {sdk}")
    runtime = private / "runtime"
    if install and not (runtime / ".qctsm-runtime.json").exists():
        archive, receipt = download(lock["vulkan"]["windows"]["runtime"], root / "downloads")
        # Keep the runtime archive hierarchy, including its original licenses.
        with zipfile.ZipFile(archive) as package:
            disk_check(private, sum(e.file_size for e in package.infolist()))
            for entry in package.infolist():
                path = safe_relative(entry.filename)
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise RuntimeError(f"Unexpected Vulkan runtime archive symlink: {entry.filename}")
                if entry.is_dir():
                    continue
                target = runtime / path
                if runtime.resolve() not in target.resolve().parents:
                    raise RuntimeError(f"Vulkan runtime destination escapes root: {target}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(entry) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
        write_json(runtime / ".qctsm-runtime.json", receipt)
        receipts.append(receipt)
        archive.unlink()
    if not runtime.is_dir():
        raise RuntimeError("Vulkan redistributable runtime is missing. Run bootstrap --install.")
    x64 = list(runtime.glob("*/x64"))
    if len(x64) != 1 or not (x64[0] / "vulkan-1.dll").is_file():
        raise RuntimeError(f"Vulkan redistributable does not contain one x64 runtime directory: {runtime}")
    return {"environment": {"VULKAN_SDK": str(sdk), "PATH": str(sdk / "Bin") + os.pathsep + os.environ.get("PATH", "")},
            "cmakeCache": {"Vulkan_ROOT": str(sdk)}, "runtimeRoots": [str(x64[0])],
            "licenseRoots": [str(runtime)], "downloads": receipts}


def prepare(variant: dict, sdk_root: Path, install: bool, lock: dict) -> dict:
    backend = variant["backend"]
    if backend in ("cpu", "metal"):
        result = {"environment": {}, "cmakeCache": {}, "runtimeRoots": [], "downloads": []}
    else:
        sdk_root.mkdir(parents=True, exist_ok=True)
        result = {"cuda": cuda_sdk, "hip": rocm_sdk, "vulkan": vulkan_sdk}[backend](variant, sdk_root, install, lock)
    if backend == "hip":
        specification = importlib.util.spec_from_file_location("qctsm_gpu_license_sources", SOURCE / "tools" / "qctsm-gpu-license-sources.py")
        license_module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(license_module)
        notices = license_module.fetch_gpu_license_sources(backend, variant["toolkitVersion"], sdk_root / "license-sources", download=install)
        if not notices.get("resources"):
            raise RuntimeError("ROCm runtime redistribution notices were not collected")
        result["additionalLicenseResources"] = notices["resources"]
    return {"schemaVersion": 1, "variant": variant["id"], "backend": backend,
            "os": variant["os"], "toolkitVersion": variant["toolkitVersion"], "gpuTargets": variant["gpuTargets"],
            "upstreamCommit": lock["upstreamCommit"], "sdkRoot": str(sdk_root), **result}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--sdk-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--output", "--report", type=Path)
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    variant = variant_by_id(args.variant)
    host = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(platform.system())
    if variant["os"] != host:
        parser.error(f"{variant['id']} requires a {variant['os']} host, current host is {host}")
    sdk_root, build_root = external(args.sdk_root), external(args.build_root)
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    report = prepare(variant, sdk_root, args.install, lock)
    output = args.output or build_root / "gpu-sdk.json"
    report["cmakeCache"]["QCOPILOTS_TOOL_SEMANTIC_MATCHING_GPU_SDK_MANIFEST"] = str(external(output))
    write_json(output, report)
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"qctsm GPU SDK: {error}", file=sys.stderr)
        raise SystemExit(1)
