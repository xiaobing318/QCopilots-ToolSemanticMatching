#!/usr/bin/env python3
"""Build the NSIS installer, with the explicit ROCm external-payload fallback.

The normal CPack NSIS generator always runs first. Only its documented 2 GiB
data-block limit permits the ROCm fallback. The portable ZIP remains a separate
CPack action. This helper never copies the installed payload to another stage.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parent.parent
NSIS_LIMIT = 2**31 - 1
ROCM_VARIANT = "windows-x64-rocm"
_selection_spec = importlib.util.spec_from_file_location(
    "qctsm_windows_package_names", SOURCE / ".github/scripts/qctsm-artifact-selection.py")
_selection = importlib.util.module_from_spec(_selection_spec)
_selection_spec.loader.exec_module(_selection)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def external(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path == SOURCE or SOURCE in path.parents or path in SOURCE.parents or path == Path(path.anchor):
        raise RuntimeError(f"Package outputs must be in an external, dedicated directory: {path}")
    return path


def safe_relative(value: str) -> str:
    path = PurePosixPath(value)
    reserved = re.compile(r"^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", re.I)
    if (not path.parts or path.is_absolute() or path.as_posix() != value or
            any(part in (".", "..") or part.endswith((" ", ".")) or reserved.match(part) or
                any(ord(ch) < 32 or ch in '<>:"\\|?*' for ch in part) for part in path.parts)):
        raise RuntimeError(f"Unsafe installed payload path: {value!r}")
    if value.casefold() == "uninstall.exe":
        raise RuntimeError("The payload cannot replace the generated uninstaller")
    return value


def payload_manifest(stage: Path, variant: str, version: str) -> dict:
    stage = stage.resolve()
    files = []
    seen = set()
    for entry in sorted(stage.rglob("*")):
        info = entry.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise RuntimeError(f"Payload cannot contain symbolic links or reparse points: {entry}")
        if entry.is_dir():
            continue
        if not stat.S_ISREG(info.st_mode) or stage not in entry.resolve().parents:
            raise RuntimeError(f"Payload contains an unsupported file: {entry}")
        relative = safe_relative(entry.relative_to(stage).as_posix())
        if relative.casefold() in seen:
            raise RuntimeError(f"Payload paths collide on Windows: {relative}")
        seen.add(relative.casefold())
        files.append({"path": relative, "bytes": info.st_size, "sha256": digest(entry)})
    if "bin/qctsm-gui.exe" not in seen:
        raise RuntimeError("CPack did not stage the application executable")
    return {"schemaVersion": 1, "variant": variant, "version": version,
            "totalBytes": sum(item["bytes"] for item in files), "files": files}


def nsis_size_failure(diagnostic: str, manifest: dict) -> bool:
    # A generic mmap error can mean RAM or disk exhaustion. It is not enough
    # by itself. With the project's SOLID compressor the uncompressed block
    # contains each distinct file plus its four-byte length and has signed
    # 32-bit offsets. Identical files can share a data-block entry, so count
    # unique content rather than treating duplicate stage files as overflow.
    # Source: NSIS Source/build.cpp:add_db_data and Source/mmap.cpp:MMapBuf.
    unique_files = {(item["bytes"], item["sha256"]) for item in manifest["files"]}
    actual_block_minimum = sum(size + 4 for size, _ in unique_files)
    if actual_block_minimum <= NSIS_LIMIT:
        return False
    if re.search(r"add_db_data\(\) called with length=-[0-9]+|"
                 r"(?:datablock|data block|installer|file)[^\n]*(?:exceeds?[^\n]*(?:limit|2\s*g)|too large)",
                 diagnostic, re.I):
        return True
    for offset, length in re.findall(r"error mmapping file \((-?[0-9]+),\s*(-?[0-9]+)\) is out of range", diagnostic, re.I):
        offset, length = int(offset), int(length)
        if offset < 0 or length < 0 or offset + length > NSIS_LIMIT:
            return True
    # A generic allocation/mmap failure provides no overflowing address. Keep
    # it as a failure even for a large package instead of concealing low disk
    # space or RAM exhaustion behind a different installer format.
    return False


def nsis_literal(value: str) -> str:
    if any(ch in value for ch in "\r\n\0"):
        raise RuntimeError("NSIS string contains a control character")
    return value.replace("$", "$$").replace('"', '$\\"')


def render_script(manifest: dict, output: Path, license_path: Path,
                  *, product_id: str = "qcopilots-tool-semantic-matching", menu: str = "QCopilots Tool Semantic Matching") -> str:
    """Render a fixed file list. The JSON beside setup.exe is never executed."""
    if not re.fullmatch(r"[a-z0-9-]+", product_id):
        raise RuntimeError("Invalid installer registry identity")
    version = manifest["version"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise RuntimeError(f"NSIS requires a three-part numeric application version: {version}")
    verify, install, uninstall = [], [], []
    directories = set()
    for item in manifest["files"]:
        path = safe_relative(item["path"])
        relative = nsis_literal(path.replace("/", "\\"))
        parent = str(PurePosixPath(path).parent)
        parent = "" if parent == "." else "\\" + nsis_literal(parent.replace("/", "\\"))
        checksum = item["sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", checksum) or type(item["bytes"]) is not int or item["bytes"] < 0:
            raise RuntimeError(f"Invalid payload identity: {path}")
        verify.append(f'  !insertmacro QCS_VERIFY "$EXEDIR\\payload\\{relative}" {item["bytes"]} "{checksum}"')
        install.extend([
            f'  StrCpy $QcsPath "$INSTDIR\\{relative}"',
            '  Call QcsCheckParents',
            '  !insertmacro QCS_REQUIRE "$QcsValid" "Unsafe destination path"',
            f'  CreateDirectory "$INSTDIR{parent}"',
            '  ClearErrors',
            f'  CopyFiles /SILENT "$EXEDIR\\payload\\{relative}" "$INSTDIR\\{relative}"',
            '  !insertmacro QCS_WRITE_OK',
            f'  !insertmacro QCS_VERIFY "$INSTDIR\\{relative}" {item["bytes"]} "{checksum}"',
        ])
        uninstall.extend([
            f'  StrCpy $QcsPath "$INSTDIR\\{relative}"',
            '  Call un.QcsCheckParents',
            '  !insertmacro QCS_REQUIRE "$QcsValid" "Unsafe installed payload path"',
            '  ClearErrors',
            f'  Delete "$INSTDIR\\{relative}"',
            '  !insertmacro QCS_WRITE_OK',
        ])
        for directory in PurePosixPath(path).parents:
            if str(directory) != ".":
                directories.add(directory.as_posix())
    for directory in sorted(directories, key=lambda value: (value.count("/"), value), reverse=True):
        uninstall.append('  RMDir "$INSTDIR\\' + nsis_literal(directory.replace("/", "\\")) + '"')
    template = (SOURCE / "cmake/nsis/ExternalPayload.nsi.in").read_text(encoding="utf-8")
    values = {"OUTPUT": nsis_literal(str(output)), "LICENSE": nsis_literal(str(license_path)),
              "ICON": nsis_literal(str(SOURCE / "resources/icons/qctsm.ico")),
              "VERSION": version, "PRODUCT_ID": product_id, "MENU": nsis_literal(menu),
              "REQUIRED_BYTES": str(manifest["totalBytes"] + 64 * 1024**2),
              "REQUIRED_KIB": str((manifest["totalBytes"] + 1023) // 1024),
              "VERIFY_PAYLOAD": "\n".join(verify), "INSTALL_PAYLOAD": "\n".join(install),
              "UNINSTALL_PAYLOAD": "\n".join(uninstall)}
    for key, value in values.items():
        template = template.replace("@" + key + "@", value)
    return template


def create_setup_zip(stage: Path, manifest: dict, output: Path, compiler: Path) -> Path:
    output = external(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Only scripts and the small setup.exe are temporary. Read the payload
    # directly from the existing CPack stage to keep peak disk usage bounded.
    with tempfile.TemporaryDirectory(prefix="qctsm-nsis-external-", dir=output.parent) as temporary:
        work = Path(temporary)
        executable = work / "setup.exe"
        script = work / "setup.nsi"
        script.write_text(render_script(manifest, executable, SOURCE / "LICENSE"), encoding="utf-8-sig")
        subprocess.run([str(compiler), "/V3", str(script)], check=True)
        partial = work / "package.zip"
        with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
            archive.write(executable, "setup.exe")
            archive.writestr("payload-manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
            for item in manifest["files"]:
                source = stage / item["path"]
                hasher = hashlib.sha256()
                count = 0
                # Hash the exact bytes streamed into the archive. A concurrent
                # stage mutation must not produce a mixed candidate package.
                with source.open("rb") as stream, archive.open("payload/" + item["path"], "w", force_zip64=True) as target:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        count += len(chunk)
                        hasher.update(chunk)
                        target.write(chunk)
                if count != item["bytes"] or hasher.hexdigest() != item["sha256"]:
                    raise RuntimeError(f"CPack payload changed while packaging: {item['path']}")
        partial.replace(output)
    return output


def cpack_values(config: Path) -> dict[str, str]:
    result = {}
    for line in config.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r'set\((CPACK_[A-Z_]+) "([^"\r\n]*)"\)', line.strip())
        if match:
            result[match[1]] = match[2].replace("\\\\", "\\")
    for name in ("CPACK_PACKAGE_FILE_NAME", "CPACK_PACKAGE_VERSION"):
        if not result.get(name):
            raise RuntimeError(f"Missing literal {name} in {config}")
    if not re.fullmatch(r"qctsm-[A-Za-z0-9._-]+", result["CPACK_PACKAGE_FILE_NAME"]):
        raise RuntimeError("Unsafe CPack package filename")
    return result


def diagnostic_stamps(package_dir: Path) -> dict[Path, tuple[int, int]]:
    return {path: (path.stat().st_mtime_ns, path.stat().st_size)
            for name in ("NSISOutput.log", "CPackError.log")
            for path in (package_dir / "_CPack_Packages").glob("**/" + name)}


def recent_diagnostics(package_dir: Path, started: float, previous: dict) -> str:
    values = []
    for name in ("NSISOutput.log", "CPackError.log"):
        for path in sorted((package_dir / "_CPack_Packages").glob("**/" + name)):
            info = path.stat()
            if (package_dir not in path.resolve().parents or info.st_mtime < started - 2 or
                    previous.get(path) == (info.st_mtime_ns, info.st_size)):
                continue
            with path.open("rb") as stream:
                stream.seek(max(0, path.stat().st_size - 256 * 1024))
                value = stream.read().decode("utf-8", errors="replace")
            print(f"NSIS diagnostic ({path}):\n{value}", flush=True)
            values.append(value)
    return "\n".join(values)


def _package_in_directory(build: Path, output: Path, config: str, variant: str, compiler: Path | None = None) -> Path:
    if os.name != "nt":
        raise RuntimeError("The Windows installer must be built on Windows")
    build, output = external(build), external(output)
    values = cpack_values(build / "CPackConfig.cmake")
    name = values["CPACK_PACKAGE_FILE_NAME"]
    if name != _selection.package_stem(variant, values["CPACK_PACKAGE_VERSION"]):
        raise RuntimeError("CPack package filename does not match the requested variant and locked SDK")
    output.mkdir(parents=True, exist_ok=True)
    previous_diagnostics = diagnostic_stamps(output)
    started = time.time()
    result = subprocess.run(["cpack", "--config", str(build / "CPackConfig.cmake"), "-C", config,
                             "-B", str(output), "-G", "NSIS"], cwd=build)
    installer = output / (name + ".exe")
    if result.returncode == 0:
        if not installer.is_file() or installer.stat().st_mtime < started - 2:
            raise RuntimeError("CPack returned success without producing the requested installer")
        # Remove only the alternate output of this exact package identity.
        (output / (name + "-setup.zip")).unlink(missing_ok=True)
        return installer
    diagnostic = recent_diagnostics(output, started, previous_diagnostics)
    if variant != ROCM_VARIANT:
        raise RuntimeError(f"CPack NSIS failed ({result.returncode}), fallback is limited to {ROCM_VARIANT}")
    stages = [path for path in (output / "_CPack_Packages").glob("*/NSIS/" + name)
              if path.is_dir() and output in path.resolve().parents]
    if len(stages) != 1:
        raise RuntimeError("CPack NSIS failure did not leave one identifiable application stage")
    manifest = payload_manifest(stages[0], variant, values["CPACK_PACKAGE_VERSION"])
    if not nsis_size_failure(diagnostic, manifest):
        raise RuntimeError(f"CPack NSIS failed ({result.returncode}) without a confirmed 2 GiB data-block overflow")
    compiler = compiler or (Path(shutil.which("makensis") or shutil.which("makensis.exe") or ""))
    if not compiler.is_file():
        raise RuntimeError("The locked NSIS compiler is required to build the external-payload installer")
    print("The Windows ROCm payload exceeded NSIS's 2 GiB data-block limit. Building setup.zip.", flush=True)
    archive = create_setup_zip(stages[0], manifest, output / (name + "-setup.zip"), compiler)
    installer.unlink(missing_ok=True)
    return archive


def package(build: Path, output: Path, config: str, variant: str, compiler: Path | None = None) -> Path:
    build, output = external(build), external(output)
    output.mkdir(parents=True, exist_ok=True)
    # CPack otherwise retains the full NSIS stage while the following portable
    # ZIP is built. Keep this invocation's stage private, then atomically move
    # only its finished user package out before removing the owned directory.
    temporary = Path(tempfile.mkdtemp(prefix="qctsm-nsis-", dir=output)).absolute()

    def check_owned_directory() -> None:
        attributes = temporary.lstat()
        if (temporary.parent != output or temporary.resolve() != temporary or
                stat.S_ISLNK(attributes.st_mode) or getattr(attributes, "st_file_attributes", 0) & 0x400):
            raise RuntimeError(f"Refusing to use or remove a redirected CPack temporary directory: {temporary}")

    def remove_readonly(function, path, failure) -> None:
        target = Path(path).absolute()
        check_owned_directory()
        if (not isinstance(failure[1], PermissionError) or function not in (os.unlink, os.rmdir) or
                (target != temporary and temporary not in target.parents) or target.resolve() != target):
            raise failure[1]
        # Some vendor runtime files retain Windows' read-only attribute after
        # installation. Change that attribute only inside this owned stage.
        target.chmod(stat.S_IWRITE)
        function(path)

    try:
        check_owned_directory()
        result = _package_in_directory(build, temporary, config, variant, compiler)
        if result.parent != temporary or result.resolve() != result or not result.is_file():
            raise RuntimeError("CPack did not return a regular package inside its owned directory")
        destination = output / result.name
        if result.name.endswith("-setup.zip"):
            alternate = output / (result.name.removesuffix("-setup.zip") + ".exe")
        elif result.suffix == ".exe":
            alternate = output / (result.stem + "-setup.zip")
        else:
            raise RuntimeError(f"Unexpected Windows installer package: {result.name}")
        os.replace(result, destination)
        alternate.unlink(missing_ok=True)
        return destination
    finally:
        # Use an explicit checked cleanup rather than a context manager that
        # would still delete on an ownership-check exception. Historical CPack
        # directories and other invocations' stages are never enumerated here.
        check_owned_directory()
        shutil.rmtree(temporary, onerror=remove_readonly)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--config", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"), default="Release")
    parser.add_argument("--variant", required=True)
    parser.add_argument("--nsis", type=Path, help="Override the private NSIS compiler path")
    args = parser.parse_args()
    print(package(args.build_dir, args.package_dir, args.config, args.variant, args.nsis), flush=True)
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"qctsm Windows package: {error}", file=sys.stderr)
        raise SystemExit(1)
