#!/usr/bin/env python3
"""Read every bundled Mach-O and reject links that depend on the build machine."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("qctsm_sign_macos",
    Path(__file__).with_name("qctsm-sign-macos.py"))
signing = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(signing)
LOADS = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
         "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}


def parse_commands(output):
    """Keep universal-image slices separate so one slice cannot repair another."""
    slices = {}
    architecture, command, found, seen = "", None, False, False

    def finish():
        if command in LOADS | {"LC_RPATH"} and not found:
            raise ValueError(f"otool omitted the path for {command}")

    for line in output.splitlines():
        header = re.fullmatch(r".+ \(architecture ([^)]+)\):", line)
        if header or line.startswith("Load command "):
            finish()
            command, found = None, False
            if header:
                architecture = header[1]
            continue
        match = re.fullmatch(r"\s*cmd (LC_[A-Z_]+)\s*", line)
        if match:
            finish()
            command, found, seen = match[1], False, True
            slices.setdefault(architecture, {"rpaths": [], "loads": []})
        elif command in LOADS | {"LC_RPATH"}:
            match = re.fullmatch(r"\s*(name|path) (.+) \(offset \d+\)\s*", line)
            if match:
                expected = "path" if command == "LC_RPATH" else "name"
                if match[1] != expected or found:
                    raise ValueError(f"Invalid otool path for {command}")
                slices[architecture]["rpaths" if command == "LC_RPATH" else "loads"].append(match[2])
                found = True
    finish()
    if not seen:
        raise ValueError("otool returned no Mach-O load commands")
    return slices


def inside(path, bundle):
    return path == bundle or bundle in path.parents


def expand_path(value, source, executable, bundle):
    anchors = {"@loader_path": source.parent, "@executable_path": executable.parent}
    for prefix, anchor in anchors.items():
        if value == prefix or value.startswith(prefix + "/"):
            result = (anchor / value[len(prefix):].lstrip("/")).resolve()
            break
    else:
        raise ValueError(f"Non-relocatable absolute or unsupported relative path: {value}")
    if not inside(result, bundle):
        raise ValueError(f"Path escapes the application: {value}")
    return result


def owner(path, bundle):
    return next((parent for parent in path.parents
                 if parent.suffix == ".app" and inside(parent, bundle)), bundle)


def verify_bundle(bundle, runner=subprocess.run):
    bundle = Path(bundle).resolve()
    report = {"schemaVersion": 1, "bundle": str(bundle),
              "policy": "static path closure using own and same-app executable rpaths, no cross-app links",
              "sources": [], "errors": [], "passed": False}
    try:
        inventory = signing.plan_bundle(bundle)
        images = {(bundle / relative).resolve(): relative for relative in inventory["machOFiles"]}
        executables = sorted({(bundle / unit["executable"]).resolve()
                              for unit in inventory["units"] if unit["fileType"] == 2})
        commands = {}
        for image, relative in images.items():
            try:
                result = runner(["/usr/bin/otool", "-arch", "all", "-l", str(image)],
                                capture_output=True, text=True, check=False, timeout=30)
                if result.returncode:
                    raise ValueError(f"otool failed: {result.stderr.strip()}")
                parsed = parse_commands(result.stdout)
                architectures = inventory["architectures"][relative]
                if set(parsed) == {""} and len(architectures) == 1:
                    parsed = {architectures[0]: parsed[""]}
                if set(parsed) != set(architectures):
                    raise ValueError("otool architecture slices do not match the Mach-O inventory")
                commands[image] = parsed
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                report["errors"].append(f"{relative}: {error}")
        for image, slices in commands.items():
            relative = images[image]
            contexts = [image] if image in executables else [exe for exe in executables
                        if owner(exe, bundle) == owner(image, bundle)]
            if not contexts:
                report["errors"].append(f"{relative}: No owning application executable")
            for architecture, data in slices.items():
                record = {"path": relative, "architecture": architecture, **data, "resolved": []}
                report["sources"].append(record)
                for executable in contexts:
                    try:
                        main = commands.get(executable, {})
                        main_data = main.get(architecture, {"rpaths": []})
                        rpaths = [expand_path(value, image, executable, bundle) for value in data["rpaths"]]
                        rpaths += [expand_path(value, executable, executable, bundle)
                                   for value in main_data["rpaths"]]
                        for load in data["loads"]:
                            if load.startswith(("/usr/lib/", "/System/Library/")):
                                # Do not allow a textual system prefix to hide traversal.
                                normalized = str(PurePosixPath(load))
                                if ".." in PurePosixPath(normalized).parts:
                                    raise ValueError(f"Noncanonical system dependency: {load}")
                                continue
                            if PurePosixPath(load).is_absolute():
                                raise ValueError(f"Non-system absolute dependency: {load}")
                            if load.startswith("@rpath/"):
                                suffix = load[len("@rpath/"):]
                                candidates = [(directory / suffix).resolve() for directory in rpaths]
                                if not candidates or any(not inside(path, bundle) for path in candidates):
                                    raise ValueError(f"Unresolved or escaping @rpath dependency: {load}")
                            else:
                                candidates = [expand_path(load, image, executable, bundle)]
                            target = next((path for path in candidates if path.is_file()), None)
                            if target not in images:
                                raise ValueError(f"Dependency does not resolve to an inventoried Mach-O: {load}")
                            if owner(target, bundle) != owner(executable, bundle):
                                raise ValueError(f"Cross-app dependency context is unsupported: {load}")
                            target_arches = inventory["architectures"][images[target]]
                            if architecture and architecture not in target_arches:
                                raise ValueError(f"Dependency lacks architecture {architecture}: {load}")
                            record["resolved"].append({"load": load, "target": images[target],
                                                       "executable": images[executable]})
                    except (OSError, ValueError) as error:
                        report["errors"].append(f"{relative} [{architecture or 'native'}, {images[executable]}]: {error}")
    except (OSError, ValueError, RuntimeError) as error:
        report["errors"].append(str(error))
    report["passed"] = bool(report["sources"]) and not report["errors"]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if signing._inside(args.report.resolve(), args.bundle.resolve()):
        raise ValueError("The link report must be outside the application")
    report = verify_bundle(args.bundle)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "sources": len(report["sources"]),
                      "errors": report["errors"], "report": str(args.report)}, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"qctsm macOS links: {error}", file=sys.stderr)
        raise SystemExit(1)
