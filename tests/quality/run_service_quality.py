#!/usr/bin/env python3
"""Launch an isolated packaged/built service and run real-model quality tests."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
from urllib.request import Request, urlopen
import uuid


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", required=True, type=Path)
    parser.add_argument("--cli", type=Path, help="Exercise the real command-line client for every labelled query")
    parser.add_argument("--model-file", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--provider-url", help="Optional real remote embedding API base instead of the embedded model")
    parser.add_argument("--timeout", type=float, default=1650, help="Whole-evaluation deadline, below the CTest timeout")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    if output == repo or repo in output.parents:
        parser.error("--output-dir must be outside the source checkout")
    if not args.service.is_file():
        parser.error("service executable is missing")
    if args.cli and not args.cli.is_file():
        parser.error("CLI executable is missing")
    if not 1 <= args.timeout <= 1650:
        parser.error("--timeout must be between 1 and 1650 seconds so cleanup fits the CTest deadline")
    model = args.model_file
    if model is None:
        root = os.environ.get("QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR")
        if root:
            model = Path(root) / "embeddinggemma-2-Q8_0.gguf"
    if not args.provider_url and (model is None or not model.is_file()):
        parser.error("real model is missing: use --model-file or QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR")
    output.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="quality-", dir=output))
    profile = run_root / "profile"
    profile.mkdir()
    token = uuid.uuid4().hex
    config = {"schema_version": 1, "provider": "http" if args.provider_url else "embedded",
              "database_path": str(run_root / "quality.sqlite3"), "model_path": str(model.resolve()) if model else "",
              "model_id": "embeddinggemma-2-Q8_0", "model_revision": "bfcd298762cc34d0357ece5ebdd31791a3a374d8",
              "backend": "cpu", "threads": max(1, min(os.cpu_count() or 1, 4)),
              "host": "127.0.0.1", "port": 0, "token": token, "timeout_ms": 120000}
    if args.provider_url:
        config["api_base"] = args.provider_url
        config["authorization"] = os.environ.get("QCTSM_QUALITY_PROVIDER_AUTHORIZATION", "")
    (profile / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    provenance = {"schema_version": 1, "service_sha256": file_sha256(args.service),
                  "cli_sha256": file_sha256(args.cli) if args.cli else None,
                  "github_sha": os.environ.get("GITHUB_SHA"), "github_run_id": os.environ.get("GITHUB_RUN_ID"),
                  "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
                  "provider": config["provider"], "requested_backend": "cpu" if not args.provider_url else None,
                  "remote_hardware_verified": False if args.provider_url else None}
    (run_root / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    process = None
    endpoint = None
    result = 1
    clean = True
    lines: queue.Queue[str | None] = queue.Queue()
    log_path = run_root / "service-stderr.log"
    with log_path.open("w", encoding="utf-8") as stderr:
        try:
            process = subprocess.Popen([str(args.service.resolve()), "--profile", str(profile), "--port", "0"],
                                       stdout=subprocess.PIPE, stderr=stderr, text=True, encoding="utf-8",
                                       errors="replace", creationflags=flags)

            def read_stdout():
                assert process is not None and process.stdout is not None
                for line in process.stdout:
                    lines.put(line)
                lines.put(None)

            reader = threading.Thread(target=read_stdout, daemon=True)
            reader.start()
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.2)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError(f"service exited during startup: {process.returncode}")
                    continue
                if line is None:
                    raise RuntimeError("service closed stdout before announcing readiness")
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if value.get("status") == "ready" and isinstance(value.get("port"), int):
                    endpoint = f"http://127.0.0.1:{value['port']}"
                    break
            if not endpoint:
                raise RuntimeError("service readiness timed out")
            report = run_root / "gis-quality.json"
            env = dict(os.environ, QCTSM_API_TOKEN=token, PYTHONDONTWRITEBYTECODE="1")
            command = [sys.executable, "-B", str(Path(__file__).with_name("run_quality.py")),
                       "--endpoint", endpoint, "--report", str(report)]
            if args.cli:
                command.extend(["--cli", str(args.cli.resolve())])
            print(json.dumps({"quality_report": str(report), "requested_backend": provenance["requested_backend"],
                              "provider": config["provider"]}), flush=True)
            result = subprocess.run(command, env=env, creationflags=flags, timeout=args.timeout).returncode
        except (OSError, RuntimeError, KeyboardInterrupt, subprocess.TimeoutExpired) as error:
            print(f"quality service failure: {error}", file=sys.stderr)
            result = 1
        finally:
            if process is not None:
                if endpoint and process.poll() is None:
                    try:
                        req = Request(endpoint + "/v1/shutdown", data=b"{}", method="POST",
                                      headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
                        with urlopen(req, timeout=5):
                            pass
                    except Exception as error:
                        print(f"service graceful shutdown request failed: {error}", file=sys.stderr)
                if process.poll() is None:
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        clean = False
                        process.terminate()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=10)
                if process.stdout is not None:
                    process.stdout.close()
                if process.returncode != 0:
                    clean = False
            # Preserve result and diagnostics, remove the ephemeral local token.
            config["token"] = "<test-token-removed>"
            config.pop("authorization", None)
            (profile / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
            (run_root / "lifecycle.json").write_text(json.dumps({"graceful_shutdown": clean,
                "service_exit_code": process.returncode if process else None,
                "evaluation_exit_code": result}, indent=2) + "\n", encoding="utf-8")
    return result if clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
