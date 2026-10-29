#!/usr/bin/env python3
"""Measure a persisted index across a real service/worker restart.

This is separate from the frozen recall benchmark. CPU model initialization is
cold after the restart, while the operating-system file cache is uncontrolled.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from run_quality import Client, ServiceError, cli_search, output_report, score
from run_service_quality import file_sha256


class OwnedService:
    def __init__(self, executable: Path, profile: Path, token: str, log: Path):
        self.executable, self.profile, self.token, self.log = executable, profile, token, log
        self.process = None
        self.client = None
        self.stderr = None
        self.graceful = False

    def __enter__(self):
        lines = queue.Queue()
        self.stderr = self.log.open("w", encoding="utf-8")
        try:
            self.process = subprocess.Popen([str(self.executable), "--profile", str(self.profile), "--port", "0"],
                stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding="utf-8", errors="replace",
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)

            def read():
                for line in self.process.stdout:
                    lines.put(line)
                lines.put(None)

            threading.Thread(target=read, daemon=True).start()
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.2)
                except queue.Empty:
                    if self.process.poll() is not None:
                        raise ServiceError("Service exited before readiness")
                    continue
                if line is None:
                    raise ServiceError("Service closed stdout before readiness")
                try:
                    ready = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if ready.get("status") == "ready" and isinstance(ready.get("port"), int):
                    self.client = Client(f"http://127.0.0.1:{ready['port']}", self.token, 30)
                    return self
            raise ServiceError("Service readiness deadline exceeded")
        except BaseException:
            self.stop()
            raise

    def stop(self):
        requested = False
        if self.process is not None:
            if self.client and self.process.poll() is None:
                try:
                    self.client.request("POST", "v1/shutdown", {})
                    requested = True
                except ServiceError:
                    pass
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=10)
            self.graceful = requested and self.process.returncode == 0
            if self.process.stdout:
                self.process.stdout.close()
        if self.stderr:
            self.stderr.close()

    def __exit__(self, exc_type, exc_value, traceback):
        self.stop()
        if not self.graceful and exc_type is None:
            raise ServiceError("Owned service or worker did not shut down normally")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--model-file", type=Path)
    parser.add_argument("--provider-url", help="Measure a fresh local connection to a remote embedding API without restarting that server")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    if output == repo or repo in output.parents:
        parser.error("Output must be outside the source checkout")
    model = args.model_file
    if model is None and os.environ.get("QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR"):
        model = Path(os.environ["QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR"]) / "embeddinggemma-2-Q8_0.gguf"
    if not args.service.is_file() or not args.cli.is_file() or (not args.provider_url and (model is None or not model.is_file())):
        parser.error("Real service, CLI and locked model files are required")
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="cold-latency-", dir=output))
    profile = run / "profile"
    profile.mkdir()
    token = uuid.uuid4().hex
    config = {"schema_version": 1, "provider": "http" if args.provider_url else "embedded", "database_path": str(run / "index.sqlite"),
        "model_path": str(model.resolve()) if model else "", "model_id": "embeddinggemma-2-Q8_0",
        "model_revision": "bfcd298762cc34d0357ece5ebdd31791a3a374d8", "backend": "cpu",
        "threads": max(1, min(os.cpu_count() or 1, 4)), "host": "127.0.0.1", "port": 0,
        "token": token, "timeout_ms": 120000}
    if args.provider_url:
        config["api_base"] = args.provider_url
        config["authorization"] = os.environ.get("QCTSM_QUALITY_PROVIDER_AUTHORIZATION", "")
    (profile / "config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
    fixture = Path(__file__).resolve().parent
    catalog = json.loads((fixture / "catalog.json").read_text(encoding="utf-8"))
    query = json.loads((fixture / "queries.json").read_text(encoding="utf-8"))["queries"][0]
    source = "qctsm-cold-" + uuid.uuid4().hex
    upload = copy.deepcopy(catalog)
    upload["source_id"] = source
    request = {"query": query["query"], "top_n": 8, "filters": {"source_id": source, "available": True}}
    cold_scope = ("Fresh local service and HTTP provider connection, existing SQLite index, remote model and server lifecycle unchanged"
                  if args.provider_url else "Fresh owned service and inference worker, existing SQLite index, uncontrolled OS file cache")
    report = {"schema_version": 1, "passed": False, "requested_backend": None if args.provider_url else "cpu", "source_id": source,
        "query_id": query["id"], "query": query["query"], "errors": [], "cleanup": "pending",
        "cold_scope": cold_scope, "provider": config["provider"],
        "service_sha256": file_sha256(args.service), "cli_sha256": file_sha256(args.cli),
        "github_sha": os.environ.get("GITHUB_SHA"), "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT")}
    prepared = None
    restarted = None
    try:
        with OwnedService(args.service.resolve(), profile, token, run / "prepare-stderr.log") as prepared:
            report["prepare_service_pid"] = prepared.process.pid
            indexed = prepared.client.completed(prepared.client.request("PUT", "v1/catalogs/" + source, upload), 300)
            if indexed.get("pending_records") != 0:
                raise ServiceError("Index did not complete all real embeddings")
            report["embedding_profile"] = indexed.get("embedding_profile")
            report["index_generation"] = indexed.get("generation")
        with OwnedService(args.service.resolve(), profile, token, run / "restart-stderr.log") as restarted:
            report["restart_service_pid"] = restarted.process.pid
            try:
                # No index job and no embedding request occurs between the new
                # process's ready message and this timed query.
                started = time.monotonic()
                cold = cli_search(restarted.client, args.cli.resolve(), request, run, 180)
                report["cold_query_seconds"] = time.monotonic() - started
                started = time.monotonic()
                warm = cli_search(restarted.client, args.cli.resolve(), request, run, 180)
                report["warm_query_seconds"] = time.monotonic() - started
                if cold.get("retrieval_mode") != "hybrid" or warm.get("retrieval_mode") != "hybrid":
                    raise ServiceError("Cold and warm queries must both use real embeddings")
                if cold.get("generation") != report["index_generation"] or warm.get("generation") != report["index_generation"]:
                    raise ServiceError("Persisted index changed during the cold/warm comparison")
                identity = lambda value: [(c.get("capability_id"), c.get("payload")) for c in value.get("candidates", [])]
                report["ranking_payload_parity"] = identity(cold) == identity(warm)
                report["cold_recall"] = score(query, cold.get("candidates", []), 8)["recall"]
                if not report["ranking_payload_parity"] or report["cold_recall"] != 1:
                    raise ServiceError("Cold/warm query result was inconsistent or missed its frozen expected capability")
            finally:
                restarted.client.cancel_active()
                empty = {"schema_version": 1, "source_id": source, "revision": "cleanup", "records": []}
                restarted.client.completed(restarted.client.request("PUT", "v1/catalogs/" + source, empty), 30)
                report["cleanup"] = "cleared"
        report["passed"] = True
    except (ServiceError, OSError, KeyboardInterrupt) as error:
        report["errors"].append(str(error))
    finally:
        report["prepare_graceful_shutdown"] = bool(prepared and prepared.graceful)
        report["restart_graceful_shutdown"] = bool(restarted and restarted.graceful)
        config["token"] = "<test-token-removed>"
        config.pop("authorization", None)
        (profile / "config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
        output_report(run / "cold-latency.json", report)
    print(json.dumps({"passed": report["passed"], "report": str(run / "cold-latency.json")}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
