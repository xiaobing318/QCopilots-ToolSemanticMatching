#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the real CLI against an isolated HTTP fixture, without model inference."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--qt-bin", required=True)
    args = parser.parse_args()
    received: list[dict] = []
    mode = {"name": "normal"}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def handle_request(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            body = json.loads(raw) if raw else None
            received.append({"method": self.command, "path": self.path,
                             "authorization": self.headers.get("Authorization"), "body": body})
            current = mode["name"]
            if current == "redirect":
                self.send_response(302)
                self.send_header("Location", "/redirect-target")
                self.end_headers()
                return
            if current == "invalid-json":
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"not JSON")
                return
            if current == "http-error":
                status, result = 503, {"status": "failed", "error_code": "fixture_unavailable"}
            elif "/v1/jobs/" in self.path:
                if current == "slow-poll":
                    time.sleep(2)
                    status, result = 200, {"status": "running", "job_id": "job-1"}
                elif current == "pending":
                    status, result = 200, {"status": "running", "job_id": "job-1"}
                elif current == "failed-job":
                    status, result = 200, {"status": "failed", "job_id": "job-1", "error_code": "fixture_failed"}
                else:
                    status, result = 200, {"status": "succeeded", "job_id": "job-1", "result": {"status": "succeeded", "candidates": [{"capability_id": "native:buffer"}]}}
            elif self.path.endswith("/v1/search") or "/v1/catalogs/" in self.path:
                status, result = 202, {"status": "queued", "job_id": "job-1"}
            elif self.path.endswith("/v1/embeddings"):
                status, result = 200, {"object": "list", "data": [], "model": "fixture"}
            else:
                status, result = 200, {"status": "ok"}
            data = json.dumps(result).encode("utf-8")
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except ConnectionError:
                # The client intentionally aborts requests when a budget expires.
                pass

        do_GET = handle_request
        do_POST = handle_request
        do_PUT = handle_request

    environment = os.environ.copy()
    for key in ("PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        environment[key] = args.qt_bin + os.pathsep + environment.get(key, "")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="qctsm-cli-test-") as temporary:
            profile = Path(temporary) / "profile"
            profile.mkdir()
            (profile / "config.json").write_text(json.dumps({"schema_version": 1,
                "token": "profile-secret", "authorization": "Bearer upstream-secret"}), encoding="utf-8")
            base = f"http://127.0.0.1:{server.server_port}/prefix/v1/"

            def run(*command: str, success: bool = True) -> dict:
                result = subprocess.run([args.cli, "--profile", str(profile), "--endpoint", base,
                    "--token", "fixture-token", *command], capture_output=True,
                    text=True, encoding="utf-8", env=environment, timeout=15)
                assert (result.returncode == 0) == success, (command, result.returncode, result.stdout, result.stderr)
                assert result.stdout, (command, result.stderr)
                return json.loads(result.stdout)

            run("health")
            assert received[-1]["path"] == "/prefix/health", received[-1]
            assert received[-1]["authorization"] == "Bearer fixture-token"
            configuration = run("config")
            assert "profile-secret" not in json.dumps(configuration)
            assert "upstream-secret" not in json.dumps(configuration)

            result = run("search", "--query", "道路缓冲区 buffer", "--top-n", "3", "--wait")
            assert result["result"]["candidates"][0]["capability_id"] == "native:buffer"
            submitted = next(row for row in received if row["path"].endswith("/v1/search"))
            assert submitted["body"]["query"] == "道路缓冲区 buffer"
            assert submitted["body"]["top_n"] == 3

            source = [{"binary_id": "bin.gdalwarp", "confirmed_risk": True, "arguments": ["-r", "lanczos", "a.tif", "b.tif"]}]
            catalog = Path(temporary) / "目录.json"
            catalog.write_text(json.dumps(source), encoding="utf-8")
            run("import", "--file", str(catalog), "--source", "catalog a")
            assert received[-1]["path"] == "/prefix/v1/catalogs/catalog%20a"
            assert received[-1]["body"] == source

            run("embeddings", "--input", "原始文本")
            assert received[-1]["body"]["input"] == "原始文本"
            strings = Path(temporary) / "strings.json"
            strings.write_text('["first", "second"]', encoding="utf-8")
            run("embeddings", "--file", str(strings))
            assert received[-1]["body"]["input"] == ["first", "second"]
            assert run("search", "--query", "x", "--top-n", "0", success=False)["error_code"] == "invalid_arguments"
            assert run("embeddings", "--input", "x", "--file", str(strings), success=False)["error_code"] == "invalid_arguments"

            mode["name"] = "failed-job"
            assert run("search", "--query", "x", "--wait", success=False)["error_code"] == "fixture_failed"
            mode["name"] = "http-error"
            assert run("health", success=False)["http_status"] == 503
            mode["name"] = "invalid-json"
            assert run("health", success=False)["error_code"] == "invalid_response"
            mode["name"] = "redirect"
            previous = len(received)
            assert run("health", success=False)["error_code"] == "redirect_rejected"
            assert len(received) == previous + 1, "CLI followed an HTTP redirect"
            mode["name"] = "pending"
            timeout = run("search", "--query", "x", "--wait", "--wait-timeout", "1", success=False)
            assert timeout["error_code"] == "wait_timeout", timeout
            assert timeout["last_snapshot"]["job_id"] == "job-1", timeout
            mode["name"] = "slow-poll"
            timeout = run("search", "--query", "x", "--wait", "--wait-timeout", "1", success=False)
            assert timeout["error_code"] == "wait_timeout", timeout
            assert timeout["last_snapshot"]["status"] == "queued", timeout
            timeout = run("search", "--query", "x", "--wait", "--timeout", "1", "--wait-timeout", "3", success=False)
            assert timeout["error_code"] == "request_timeout", timeout
            assert not any(row["path"].endswith("/cancel") for row in received), "Wait timeout implicitly cancelled a job"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("qctsm CLI protocol tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
