#!/usr/bin/env python3
"""Evaluate a running qctsm service with real embeddings and frozen GIS labels.

The service should use an isolated test profile. This runner uploads only a
unique benchmark source, filters every query to that source, and clears that
source when finished. It never loads a user's QGIS project or executes a tool.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen
import uuid


class ServiceError(RuntimeError):
    pass


class Client:
    def __init__(self, endpoint: str, token: str, timeout: float):
        parsed = urlsplit(endpoint)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError("endpoint must be an HTTP(S) base URL without credentials, query or fragment")
        self.endpoint = endpoint.rstrip("/") + "/"
        self.token, self.timeout = token, timeout
        self.active_job: str | None = None

    def request(self, method: str, route: str, body=None):
        headers = {"Accept": "application/json"}
        data = None
        if body is not None:
            headers["Content-Type"] = "application/json; charset=utf-8"
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = Request(self.endpoint + route.lstrip("/"), data=data, headers=headers, method=method)
        try:
            with urlopen(req, timeout=self.timeout) as response:
                value = json.load(response)
        except HTTPError as error:
            detail = error.read(4096).decode("utf-8", errors="replace")
            raise ServiceError(f"HTTP {error.code}: {detail}") from error
        except (URLError, TimeoutError, OSError, json.JSONDecodeError) as error:
            raise ServiceError(f"{method} {route}: {error}") from error
        if not isinstance(value, dict):
            raise ServiceError(f"{method} {route} returned a non-object JSON response")
        return value

    def completed(self, value: dict, deadline_seconds: float):
        started = time.monotonic()
        job_id = value.get("job_id")
        if job_id:
            self.active_job = str(job_id)
        while value.get("status") in ("queued", "running", "cancelling"):
            if not job_id:
                raise ServiceError("Asynchronous response is missing job_id")
            if time.monotonic() - started >= deadline_seconds:
                self.cancel_active()
                raise ServiceError(f"Job exceeded {deadline_seconds:g} seconds")
            time.sleep(0.1)
            value = self.request("GET", "v1/jobs/" + quote(str(job_id), safe=""))
        self.active_job = None
        if value.get("status") != "succeeded":
            raise ServiceError("Job did not succeed: " + json.dumps(value, ensure_ascii=False))
        result = value.get("result", value)
        if not isinstance(result, dict) or result.get("status", "succeeded") != "succeeded":
            raise ServiceError("Operation failed: " + json.dumps(result, ensure_ascii=False))
        return result

    def cancel_active(self):
        if self.active_job:
            job = self.active_job
            self.active_job = None
            self.request("POST", "v1/jobs/" + quote(job, safe="") + "/cancel", {})


def cli_search(client: Client, executable: Path, request: dict, report_dir: Path, deadline: float):
    """Exercise the shipped CLI without exposing its token in process arguments."""
    report_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="qctsm-cli-", dir=report_dir) as profile:
        config = Path(profile) / "config.json"
        config.write_text(json.dumps({"schema_version": 1, "token": client.token}), encoding="utf-8")
        try:
            config.chmod(0o600)
            command = [str(executable), "--profile", profile, "--endpoint", client.endpoint,
                       "search", "--query", request["query"], "--top-n", str(request["top_n"]),
                       "--source", request["filters"]["source_id"], "--available-only", "--wait",
                       "--wait-timeout", str(max(1, int(deadline))),
                       "--timeout", str(max(1, int(client.timeout)))]
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=deadline + client.timeout + 5,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            value = json.loads(result.stdout)
            if not isinstance(value, dict):
                raise ServiceError("CLI returned a non-object JSON response")
            if result.returncode:
                if value.get("job_id"):
                    client.active_job = str(value["job_id"])
                    client.cancel_active()
                raise ServiceError("CLI failed: " + json.dumps(value, ensure_ascii=False))
            return client.completed(value, deadline)
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            raise ServiceError(f"CLI search failed: {error}") from error


def json_bytes(value) -> int:
    """Exact compact UTF-8 size, deliberately not an estimated token count."""
    return len(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


def identities(candidate: dict) -> set[str]:
    """Read native identity fields, never infer relevance from descriptions."""
    values: set[str] = set()
    for key in ("capability_id", "record_id", "algorithm_id", "binary_id", "tool_name", "id", "name"):
        if isinstance(candidate.get(key), str):
            values.add(candidate[key])
    for key in ("payload", "metadata", "route", "invocation"):
        if isinstance(candidate.get(key), dict):
            values.update(identities(candidate[key]))
    return values


def matches(expected: str, values: set[str]) -> bool:
    # The QGIS adapter may namespace a native ID by its service/category.
    return any(value == expected or value.endswith("/" + expected) or value.endswith(":" + expected)
               for value in values)


def score(query: dict, candidates: list[dict], top_n: int) -> dict:
    expected = query["expected"]
    ranked = [identities(candidate) for candidate in candidates[:top_n]]
    found = [item for item in expected if any(matches(item, values) for values in ranked)]
    first = next((rank for rank, values in enumerate(ranked, 1)
                  if any(matches(item, values) for item in expected)), None)
    return {"expected": expected, "found": found, "missing": sorted(set(expected) - set(found)),
            "recall": len(found) / len(expected), "reciprocal_rank": 1 / first if first else 0,
            "ranked": [candidate.get("capability_id", candidate.get("record_id")) for candidate in candidates[:top_n]]}


def percentile(values: list[float], percentile_value: float):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values) * percentile_value) - 1)]


def output_report(path: Path, report: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def evaluate(args) -> int:
    root = Path(__file__).resolve().parents[2]
    report_path = args.report.resolve()
    if report_path == root or root in report_path.parents:
        raise ValueError("--report must be outside the source checkout")
    # Git checkouts on Windows and POSIX must identify the same frozen input.
    # Only transport line endings are canonicalized, never JSON values/labels.
    catalog_bytes = args.catalog.read_bytes().replace(b"\r\n", b"\n")
    query_bytes = args.queries.read_bytes().replace(b"\r\n", b"\n")
    catalog, dataset = json.loads(catalog_bytes), json.loads(query_bytes)
    queries = dataset["queries"]
    if len(queries) < 50 or any(not item.get("expected") for item in queries):
        raise ValueError("Acceptance requires at least 50 queries with non-empty expected IDs")
    top_n = dataset.get("top_n", 8)
    minimum = dataset.get("minimum_recall", 0.90)
    if top_n != 8 or minimum < 0.90:
        raise ValueError("Acceptance dataset must use top_n=8 and minimum_recall >= 0.90")
    client = Client(args.endpoint, os.environ.get(args.token_env, ""), args.request_timeout)
    if args.cli is not None and not args.cli.is_file():
        raise ValueError("--cli must point to the built or installed qctsm-cli executable")
    source_id = "qctsm-quality-" + uuid.uuid4().hex
    upload = copy.deepcopy(catalog)
    upload["source_id"] = source_id
    report = {"schema_version": 1, "dataset_id": dataset["dataset_id"], "dataset_revision": dataset["revision"],
              "hash_normalization": "utf8-lf",
              "catalog_sha256": hashlib.sha256(catalog_bytes).hexdigest(),
              "queries_sha256": hashlib.sha256(query_bytes).hexdigest(),
              "record_count": len(catalog["records"]), "query_count": len(queries),
              "top_n": top_n, "minimum_recall": minimum, "source_id": source_id,
              "started_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "service": {}, "queries": [], "errors": [], "cleanup": "pending", "passed": False}
    report["query_transport"] = "qctsm-cli" if args.cli else "http"
    report["latency_scope"] = "Complete query transport, including CLI startup when selected. Indexing may already have loaded the model."
    report["context_size"] = {"unit": "compact_json_utf8_bytes", "full_catalog_payloads":
                              json_bytes([record["payload"] for record in catalog["records"]])}
    imported = False
    interrupted = False
    try:
        report["service"] = client.request("GET", "health")
        index_started = time.monotonic()
        imported = True
        try:
            sync = client.completed(client.request("PUT", "v1/catalogs/" + source_id, upload), args.index_timeout)
        finally:
            report["index_seconds"] = time.monotonic() - index_started
        report["index_result"] = sync
        if sync.get("pending_records") != 0:
            raise ServiceError("Index task returned without confirming that every embedding is complete")
        for query in queries:
            started = time.monotonic()
            row = {"id": query["id"], "language": query["language"], "query": query["query"]}
            try:
                request = {
                    "query": query["query"], "top_n": top_n, "filters": {"source_id": source_id, "available": True}
                }
                result = (cli_search(client, args.cli, request, report_path.parent, args.query_timeout)
                          if args.cli else client.completed(client.request("POST", "v1/search", request), args.query_timeout))
                row["latency_seconds"] = time.monotonic() - started
                mode = result.get("retrieval_mode")
                if mode != "hybrid":
                    raise ServiceError(f"Real-embedding acceptance requires hybrid retrieval, got {mode!r}")
                candidates = result.get("candidates")
                if not isinstance(candidates, list) or any(not isinstance(c, dict) for c in candidates):
                    raise ServiceError("Search response has invalid candidates")
                if any(not isinstance(item.get("semantic_score"), (int, float))
                       or isinstance(item.get("semantic_score"), bool)
                       or not math.isfinite(item["semantic_score"]) for item in candidates):
                    raise ServiceError("A candidate has no finite semantic score despite completed indexing")
                row.update(score(query, candidates, top_n))
                row["retrieval_mode"] = mode
                row["candidate_payloads_bytes"] = json_bytes([candidate.get("payload") for candidate in candidates])
                row["candidate_response_bytes"] = json_bytes(candidates)
                if args.cli and not report["queries"]:
                    parity_started = time.monotonic()
                    direct = client.completed(client.request("POST", "v1/search", request), args.query_timeout)
                    report["cli_http_parity_seconds"] = time.monotonic() - parity_started
                    # Both paths query the same immutable catalogue and profile.
                    def comparable(value):
                        return [(item.get("capability_id"), item.get("record_id"), item.get("payload"),
                                 item.get("matched_records")) for item in value.get("candidates", [])]
                    report["cli_http_parity"] = (direct.get("retrieval_mode") == "hybrid"
                                                 and comparable(result) == comparable(direct))
                    if not report["cli_http_parity"]:
                        raise ServiceError("CLI and HTTP returned different ranking or raw JSON")
                if "embedding_profile" in result:
                    report["embedding_profile"] = result["embedding_profile"]
            except ServiceError as error:
                row.update({"expected": query["expected"], "found": [], "missing": query["expected"],
                            "recall": 0.0, "reciprocal_rank": 0.0, "ranked": [], "error": str(error)})
            row.setdefault("latency_seconds", time.monotonic() - started)
            report["queries"].append(row)
            print(json.dumps({"id": row["id"], "recall": row["recall"], "seconds": round(row["latency_seconds"], 3),
                              "error": row.get("error")}, ensure_ascii=False), flush=True)
            output_report(report_path, report)
        # Repeat one unchanged query to distinguish cache/warm behavior from
        # first-request latency. It is not an extra labelled success.
        started = time.monotonic()
        warm_request = {
            "query": queries[0]["query"], "top_n": top_n, "filters": {"source_id": source_id, "available": True}
        }
        warm = (cli_search(client, args.cli, warm_request, report_path.parent, args.query_timeout)
                if args.cli else client.completed(client.request("POST", "v1/search", warm_request), args.query_timeout))
        report["warm_repeat_seconds"] = time.monotonic() - started
        report["warm_repeat_mode"] = warm.get("retrieval_mode")
        if warm.get("retrieval_mode") != "hybrid":
            report["errors"].append("warm repeat did not use real embeddings")
    except KeyboardInterrupt:
        interrupted = True
        report["errors"].append("Interrupted by the operator")
    except (ServiceError, ValueError, KeyError) as error:
        report["errors"].append(str(error))
    finally:
        try:
            client.cancel_active()
            if imported and not args.keep_catalog:
                empty = {"schema_version": 1, "source_id": source_id, "revision": "cleanup", "records": []}
                client.completed(client.request("PUT", "v1/catalogs/" + source_id, empty), args.index_timeout)
                report["cleanup"] = "cleared"
            else:
                report["cleanup"] = "retained" if imported else "not_needed"
        except ServiceError as error:
            report["cleanup"] = "failed"
            report["errors"].append("Benchmark source cleanup failed: " + str(error))
        rows = report["queries"]
        # A failed/unrun query still contributes zero through the fixed divisor.
        report["recall_at_8"] = sum(row["recall"] for row in rows) / len(queries)
        report["mrr_at_8"] = sum(row["reciprocal_rank"] for row in rows) / len(queries)
        latencies = [row["latency_seconds"] for row in rows]
        report["latency_seconds"] = {"first_query": latencies[0] if latencies else None,
                                     "median": statistics.median(latencies) if latencies else None,
                                     "p95": percentile(latencies, 0.95)}
        context_sizes = [row["candidate_payloads_bytes"] for row in rows if "candidate_payloads_bytes" in row]
        report["context_size"]["candidate_payloads_median"] = statistics.median(context_sizes) if context_sizes else None
        completed_ids = {row["id"] for row in rows}
        report["unrun_queries"] = [query["id"] for query in queries if query["id"] not in completed_ids]
        report["failures"] = [row["id"] for row in rows if row["recall"] < 1.0 or row.get("error")] + report["unrun_queries"]
        report["passed"] = (len(rows) == len(queries) and report["recall_at_8"] >= minimum
                            and not report["errors"] and not any(row.get("error") for row in rows))
        report["finished_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        output_report(report_path, report)
    print(json.dumps({"passed": report["passed"], "recall_at_8": report["recall_at_8"],
                      "mrr_at_8": report["mrr_at_8"], "report": str(report_path)}, ensure_ascii=False))
    return 130 if interrupted else (0 if report["passed"] else 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    fixture_dir = Path(__file__).resolve().parent
    parser.add_argument("--endpoint", default="http://127.0.0.1:8383")
    parser.add_argument("--token-env", default="QCTSM_API_TOKEN")
    parser.add_argument("--catalog", type=Path, default=fixture_dir / "catalog.json")
    parser.add_argument("--queries", type=Path, default=fixture_dir / "queries.json")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--cli", type=Path, help="Use the real qctsm-cli for queries and verify HTTP ranking/payload parity")
    parser.add_argument("--request-timeout", type=float, default=60)
    parser.add_argument("--index-timeout", type=float, default=1800)
    parser.add_argument("--query-timeout", type=float, default=180)
    parser.add_argument("--keep-catalog", action="store_true", help="Retain the unique benchmark source for inspection")
    args = parser.parse_args()
    try:
        return evaluate(args)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        parser.error(str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
