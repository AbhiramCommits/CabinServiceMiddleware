#!/usr/bin/env python3
"""Cabin log analysis and incident reproduction CLI (stdlib only).

Subcommands:
  trace <trace_id> <logs...>   ordered cross-service timeline + slowest span
  stats <logs...> [--json]     call counts, error codes, latency percentiles
  triage <log>                 classify failures and emit a markdown table
  repro <capture>              replay a candump capture through a fresh stack
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
ERROR_NAMES = {
    0: "OK",
    1: "INVALID_ARGUMENT",
    2: "OUT_OF_RANGE",
    3: "TRUNCATED",
    4: "BAD_MAGIC",
    5: "BAD_CRC",
    6: "VERSION_MISMATCH",
    7: "TIMEOUT",
    8: "NOT_FOUND",
    9: "UNAVAILABLE",
    10: "QUEUE_OVERFLOW",
    11: "IO",
    12: "PARSE",
    13: "UNSUPPORTED",
    14: "ALREADY_EXISTS",
    15: "INTERNAL",
}
MAGIC = b"CBN1"


def die(message: str) -> "NoReturn":
    print(f"logtool: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def iter_files(paths: Iterable[str]) -> List[Path]:
    files: List[Path] = []
    for entry in paths:
        path = Path(entry)
        if str(entry) == "-":
            files.append(path)
        elif path.is_dir():
            files.extend(sorted(p for p in path.rglob("*") if p.is_file()))
        elif path.exists():
            files.append(path)
        else:
            print(f"logtool: warning: {entry} does not exist", file=sys.stderr)
    return files


def read_text(path: Path) -> str:
    if str(path) == "-":
        return sys.stdin.read()
    return path.read_text(encoding="utf-8", errors="replace")


def parse_log_lines(paths: Iterable[str]) -> List[Dict[str, Any]]:
    records: List[Dict[str, Any]] = []
    for path in iter_files(paths):
        for line in read_text(path).splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(record, dict) and "ts_us" in record:
                records.append(record)
    records.sort(key=lambda item: item.get("ts_us", 0))
    return records


def field(record: Dict[str, Any], key: str, fallback: Any = "") -> Any:
    fields = record.get("fields")
    if isinstance(fields, dict) and key in fields:
        return fields[key]
    return fallback


def percentile(values: List[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(math.floor(index))
    upper = min(lower + 1, len(ordered) - 1)
    weight = index - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


# ---------------------------------------------------------------------------
# trace
# ---------------------------------------------------------------------------


def command_trace(args: argparse.Namespace) -> int:
    records = parse_log_lines(args.logs)
    try:
        wanted = f"{int(args.trace_id, 16):016x}"
    except ValueError:
        wanted = args.trace_id.lower()
    matching = [
        record for record in records if str(record.get("trace_id", "")).lower() == wanted
    ]
    if not matching:
        print(f"trace {wanted}: no records found")
        return 1

    origin = matching[0]["ts_us"]
    spans: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for record in matching:
        span_id = field(record, "span") or record.get("span", "")
        if not span_id:
            continue
        key = (str(record.get("service")), str(span_id))
        entry = spans.setdefault(
            key,
            {
                "service": record.get("service"),
                "name": field(record, "span_name"),
                "start": record["ts_us"],
                "duration": None,
            },
        )
        if record.get("event") == "span_end":
            entry["duration"] = float(field(record, "duration_us", 0.0))

    print(
        f"trace {wanted}: {len(matching)} records across "
        f"{len({r.get('service') for r in matching})} services"
    )
    print(f"{'t+us':>10}  {'service':<18} {'event':<18} {'span name':<24} {'dur_us':>8}")
    for record in matching:
        span_id = field(record, "span") or record.get("span", "")
        key = (str(record.get("service")), str(span_id))
        span = spans.get(key, {})
        duration = span.get("duration") if record.get("event") == "span_end" else None
        print(
            f"{record['ts_us'] - origin:>10}  {str(record.get('service')):<18} "
            f"{str(record.get('event')):<18} {str(span.get('name') or ''):<24} "
            f"{(f'{duration:.1f}' if duration is not None else ''):>8}"
        )

    completed = [
        (key, span) for key, span in spans.items() if span.get("duration") is not None
    ]
    leaf = []
    for key, span in completed:
        end = span["start"] + span["duration"]
        contains_child = False
        for other_key, other in completed:
            if other_key == key:
                continue
            other_end = other["start"] + other["duration"]
            if other["start"] >= span["start"] and other_end <= end:
                contains_child = True
                break
        if not contains_child:
            leaf.append((key, span))
    candidates = leaf or completed
    if candidates:
        _, slowest = max(candidates, key=lambda item: item[1]["duration"])
        print(
            f"\nslowest hop: {slowest['service']}::{slowest['name']} "
            f"{slowest['duration']:.1f} us"
        )
    return 0


# ---------------------------------------------------------------------------
# stats
# ---------------------------------------------------------------------------


def command_stats(args: argparse.Namespace) -> int:
    records = parse_log_lines(args.logs)
    services: Dict[str, Dict[str, Any]] = {}
    error_codes: Dict[str, int] = {}
    durations: List[float] = []

    for record in records:
        service = str(record.get("service", "unknown"))
        entry = services.setdefault(
            service, {"records": 0, "errors": 0, "durations": []}
        )
        entry["records"] += 1
        level = record.get("level")
        event = str(record.get("event", ""))
        is_error = level == "ERROR" or any(
            token in event for token in ("error", "failed", "rejected", "out_of_range")
        )
        if is_error:
            entry["errors"] += 1
        code = field(record, "error_code", None)
        if isinstance(code, (int, float)):
            error_codes[ERROR_NAMES.get(int(code), str(int(code)))] = (
                error_codes.get(ERROR_NAMES.get(int(code), str(int(code))), 0) + 1
            )
        if isinstance(field(record, "status", None), (int, float)):
            status = int(field(record, "status"))
            if status >= 400:
                key = f"HTTP_{status}"
                error_codes[key] = error_codes.get(key, 0) + 1
        if record.get("event") == "span_end":
            duration = float(field(record, "duration_us", 0.0))
            entry["durations"].append(duration)
            durations.append(duration)

    summary: Dict[str, Any] = {
        "records": len(records),
        "error_codes": error_codes,
        "latency_us": {
            "count": len(durations),
            "p50": round(percentile(durations, 0.50), 1),
            "p95": round(percentile(durations, 0.95), 1),
            "p99": round(percentile(durations, 0.99), 1),
            "max": round(max(durations), 1) if durations else 0.0,
        },
        "services": {},
    }
    for name, entry in sorted(services.items()):
        summary["services"][name] = {
            "records": entry["records"],
            "errors": entry["errors"],
            "spans": len(entry["durations"]),
            "p50_us": round(percentile(entry["durations"], 0.50), 1),
            "p95_us": round(percentile(entry["durations"], 0.95), 1),
            "p99_us": round(percentile(entry["durations"], 0.99), 1),
        }

    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0

    print(f"{len(records)} records from {len(summary['services'])} services")
    print(f"{'service':<20} {'records':>8} {'errors':>7} {'spans':>7} {'p50_us':>9} "
          f"{'p95_us':>9} {'p99_us':>9}")
    for name, entry in summary["services"].items():
        print(
            f"{name:<20} {entry['records']:>8} {entry['errors']:>7} "
            f"{entry['spans']:>7} {entry['p50_us']:>9} {entry['p95_us']:>9} "
            f"{entry['p99_us']:>9}"
        )
    latency = summary["latency_us"]
    print(
        f"\nspan latency: p50={latency['p50']} us p95={latency['p95']} us "
        f"p99={latency['p99']} us max={latency['max']} us"
    )
    print("error codes: " + (", ".join(
        f"{name}={count}" for name, count in sorted(error_codes.items())
    ) or "none"))
    return 0


# ---------------------------------------------------------------------------
# triage
# ---------------------------------------------------------------------------


def classify(text: str) -> Tuple[str, str]:
    sanitizer_patterns = [
        (r"ThreadSanitizer:\s*(.+)", "SANITIZER(tsan)"),
        (r"AddressSanitizer:\s*(.+)", "SANITIZER(asan)"),
        (r"UndefinedBehaviorSanitizer:\s*(.+)", "SANITIZER(ubsan)"),
        (r"LeakSanitizer:\s*(.+)", "SANITIZER(lsan)"),
        (r"runtime error:", "SANITIZER(ubsan)"),
    ]
    for pattern, label in sanitizer_patterns:
        if re.search(pattern, text):
            return label, pattern
    if re.search(r"VERSION_MISMATCH|version mismatch", text, re.IGNORECASE):
        return "VERSION_MISMATCH", r"VERSION_MISMATCH"
    if "signal_out_of_range" in text:
        return "SIGNAL_OUT_OF_RANGE", r"signal_out_of_range"
    if re.search(r"Timeout|timed out|TIMEOUT", text):
        return "TIMEOUT", r"Timeout|timed out|TIMEOUT"
    if re.search(r"Segmentation fault|Subprocess aborted|core dumped|SIGSEGV",
                 text, re.IGNORECASE):
        return "CRASH", r"Segmentation fault|Subprocess aborted|core dumped"
    return "ASSERT", r"Failure|FAILED"


def command_triage(args: argparse.Namespace) -> int:
    text = read_text(Path(args.log)) if args.log != "-" else sys.stdin.read()
    test_names: List[str] = []
    for pattern in (
        r"\[\s+FAILED\s+\]\s+([^\s]+)",
        r"Test\s+#\d+:\s+(.+?)\s+\.*\s+Failed",
        r"^\s*\d+\s+-\s+(.+)$",
    ):
        for match in re.finditer(pattern, text, re.MULTILINE):
            name = match.group(1).strip()
            if name and name not in test_names:
                test_names.append(name)

    classification, _ = classify(text)
    services = sorted(set(re.findall(r'"service":"([^"]+)"', text)))
    traces = sorted(set(re.findall(r'"trace_id":"([0-9a-fA-F]{16})"', text)))
    evidence = ""
    for line in text.splitlines():
        if any(token in line for token in
               ("Sanitizer", "FAILED", "Failure", "signal_out_of_range",
                "VERSION_MISMATCH", "Segmentation", "Subprocess aborted")):
            evidence = line.strip()[:220]
            break
    if not test_names:
        test_names = ["(no test name in log)"]

    entries = []
    for name in test_names:
        entries.append(
            {
                "test": name,
                "classification": classification,
                "service": services[0] if services else "unknown",
                "services": services,
                "trace_id": traces[0] if traces else "",
                "evidence": evidence,
            }
        )

    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "triage.json").write_text(
        json.dumps({"entries": entries}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print("| Test / failure | Classification | Service | Trace | Evidence |")
    print("| --- | --- | --- | --- | --- |")
    for entry in entries:
        trace = f"`{entry['trace_id']}`" if entry["trace_id"] else "-"
        print(
            f"| `{entry['test']}` | {entry['classification']} | "
            f"{entry['service']} | {trace} | {entry['evidence'] or '-'} |"
        )
    return 0


# ---------------------------------------------------------------------------
# repro: minimal IDL client + fresh stack
# ---------------------------------------------------------------------------


def load_schema() -> Dict[str, Any]:
    return json.loads((ROOT / "generated" / "cabin_schema.json").read_text())


def encode_frame(schema: Dict[str, Any], service: int, method: int, seq: int,
                 trace: int, payload: bytes, message_type: int = 1) -> bytes:
    version = schema["version"]
    header = bytearray()
    header += MAGIC
    header += struct.pack("<BBBB", version["major"], version["minor"],
                          message_type, 0)
    header += struct.pack("<HH", service, method)
    header += struct.pack("<I", seq)
    header += struct.pack("<Q", trace)
    header += struct.pack("<I", len(payload))
    body = bytes(header) + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def recv_frame(sock: socket.socket) -> Optional[Dict[str, Any]]:
    header = b""
    while len(header) < 28:
        chunk = sock.recv(28 - len(header))
        if not chunk:
            return None
        header += chunk
    if header[:4] != MAGIC:
        raise ValueError("bad frame magic from broker")
    major, minor, message_type, flags = struct.unpack("<BBBB", header[4:8])
    service, method = struct.unpack("<HH", header[8:12])
    seq, trace = struct.unpack("<IQ", header[12:24])
    (length,) = struct.unpack("<I", header[24:28])
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise ValueError("truncated frame payload")
        payload += chunk
    crc = b""
    while len(crc) < 4:
        chunk = sock.recv(4 - len(crc))
        if not chunk:
            raise ValueError("truncated frame crc")
        crc += chunk
    expected = struct.unpack("<I", crc)[0]
    if zlib.crc32(header + payload) & 0xFFFFFFFF != expected:
        raise ValueError("frame crc mismatch")
    return {
        "major": major,
        "minor": minor,
        "type": message_type,
        "service": service,
        "method": method,
        "seq": seq,
        "trace": trace,
        "payload": payload,
    }


def decode_snapshot(schema: Dict[str, Any], payload: bytes) -> Dict[str, Any]:
    fields = schema["structs"]["SignalSnapshot"]["fields"]
    values: Dict[str, Any] = {}
    offset = 0
    for item in fields:
        type_name = item["type"]
        if type_name == "u16":
            (value,) = struct.unpack_from("<H", payload, offset)
            offset += 2
        elif type_name == "u8":
            (value,) = struct.unpack_from("<B", payload, offset)
            offset += 1
        elif type_name == "f32":
            (value,) = struct.unpack_from("<f", payload, offset)
            offset += 4
        elif type_name == "u64":
            (value,) = struct.unpack_from("<Q", payload, offset)
            offset += 8
        elif type_name.startswith("enum:"):
            (raw,) = struct.unpack_from("<B", payload, offset)
            offset += 1
            enum_name = type_name.split(":", 1)[1]
            values_list = schema["enums"][enum_name]["values"]
            value = values_list[raw] if raw < len(values_list) else f"INVALID({raw})"
        else:
            raise ValueError(f"unsupported snapshot field {type_name}")
        values[item["name"]] = value
    return values


def wait_for_service(sock: socket.socket, schema: Dict[str, Any],
                     service_id: int, timeout: float = 5.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.sendall(encode_frame(schema, service_id, 0, 0, 1, b"", message_type=6))
        sock.settimeout(0.2)
        try:
            frame = recv_frame(sock)
        except (socket.timeout, ValueError):
            continue
        if frame and frame["type"] == 7 and frame["service"] == service_id:
            sock.settimeout(2.0)
            return True
    sock.settimeout(2.0)
    return False


def query_signals(schema: Dict[str, Any], socket_path: Path) -> Dict[str, Any]:
    signal_service = next(
        service for service in schema["services"]
        if service["name"] == "SignalService"
    )
    service_id = signal_service["id"]
    method_id = next(
        method["id"] for method in signal_service["methods"]
        if method["name"] == "GetSignals"
    )
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect(str(socket_path))
    try:
        if not wait_for_service(sock, schema, service_id):
            raise RuntimeError("SignalService was not discovered")
        sock.sendall(encode_frame(schema, service_id, method_id, 42,
                                  0x800000000000EE01, b""))
        for _ in range(100):
            frame = recv_frame(sock)
            if frame is None:
                break
            if frame["seq"] == 42 and frame["type"] == 2:
                return decode_snapshot(schema, frame["payload"])
            if frame["seq"] == 42 and frame["type"] == 3:
                raise RuntimeError("SignalService GetSignals failed")
    finally:
        sock.close()
    raise RuntimeError("no GetSignals response")


def command_repro(args: argparse.Namespace) -> int:
    capture = Path(args.capture)
    if not capture.exists():
        die(f"capture {capture} does not exist")
    expect_path = Path(args.expect) if args.expect else Path(
        str(capture.with_suffix("")) + ".expect.json"
    )
    if not expect_path.exists():
        die(f"expectation file {expect_path} does not exist")
    expectations = json.loads(expect_path.read_text())
    schema = load_schema()
    build_dir = Path(args.build_dir)

    temporary = Path(tempfile.mkdtemp(prefix="cabin-repro-"))
    runtime = temporary / "runtime"
    state = temporary / "state"
    logs = temporary / "logs"
    for directory in (runtime, state, logs):
        directory.mkdir(parents=True, exist_ok=True)
    log_path = logs / "stack.jsonl"
    environment = dict(
        os.environ,
        CABIN_RUNTIME_DIR=str(runtime),
        CABIN_STATE_DIR=str(state),
        CABIN_LOG_LEVEL="info",
        CABIN_LOG_FILE=str(log_path),
    )
    processes: List[subprocess.Popen] = []
    try:
        broker = subprocess.Popen(
            [str(build_dir / "cabind"), "--runtime-dir", str(runtime)],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        processes.append(broker)
        signald = subprocess.Popen(
            [str(build_dir / "signald"), "--source", f"replay:{capture}",
             "--runtime-dir", str(runtime)],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        processes.append(signald)
        socket_path = runtime / "broker.sock"
        for _ in range(100):
            if socket_path.exists():
                break
            time.sleep(0.05)
        for _ in range(100):
            if log_path.exists() and "replay_finished" in read_text(log_path):
                break
            time.sleep(0.05)
        snapshot = query_signals(schema, socket_path)
    finally:
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
        log_text = read_text(log_path) if log_path.exists() else ""

    stale = set()
    warning = "signal_out_of_range" in log_text
    for line in log_text.splitlines():
        if not line.startswith("{"):
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        summary = field(record, "summary", "")
        if isinstance(summary, str):
            for name in re.findall(r"(\w+) .*\[STALE\]", summary):
                stale.add(name)

    failures: List[str] = []
    assertions: List[Dict[str, Any]] = []
    for name, expected in expectations.get("expected", {}).items():
        actual = snapshot.get(name)
        if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
            ok = abs(float(actual) - float(expected)) <= 0.01
        else:
            ok = actual == expected
        assertions.append(
            {"assertion": f"{name} == {expected}", "actual": actual, "ok": ok}
        )
        if not ok:
            failures.append(f"{name}: expected {expected}, got {actual}")
    for name in expectations.get("stale", []):
        ok = name in stale
        assertions.append({"assertion": f"{name} is stale", "actual": ok, "ok": ok})
        if not ok:
            failures.append(f"{name}: expected stale, but it was fresh")
    required_warning = expectations.get("requires_warning")
    if required_warning:
        ok = required_warning in log_text
        assertions.append(
            {"assertion": f"warning {required_warning} emitted", "actual": ok, "ok": ok}
        )
        if not ok:
            failures.append(f"missing warning {required_warning}")

    guard = expectations.get("guard", "")
    if guard:
        setpoint = snapshot.get("hvac_setpoint_c")
        guard_ok = isinstance(setpoint, (int, float)) and 16.0 <= float(setpoint) <= 30.0
        assertions.append(
            {"assertion": guard, "actual": setpoint, "ok": guard_ok}
        )
        if not guard_ok:
            failures.append(f"guard failed: {guard} (actual {setpoint})")

    expect_reproduced = bool(expectations.get("expect_symptom_reproduced", False))
    reproduced = bool(failures)
    outcome_matches = reproduced == expect_reproduced

    print(f"symptom: {expectations.get('symptom', 'unspecified')}")
    print(f"capture: {capture}")
    print(f"snapshot: {json.dumps(snapshot, sort_keys=True)}")
    print("assertions:")
    for assertion in assertions:
        print(
            f"  [{'PASS' if assertion['ok'] else 'FAIL'}] {assertion['assertion']}"
            f" (actual={assertion['actual']})"
        )
    if failures:
        print(f"failing assertion: {failures[0]}")
    else:
        print("failing assertion: none (guard held)")
    print(
        f"symptom reproduced: {'yes' if reproduced else 'no'} "
        f"(expected {'yes' if expect_reproduced else 'no'})"
    )
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "repro.json").write_text(
        json.dumps(
            {
                "capture": str(capture),
                "symptom": expectations.get("symptom"),
                "reproduced": reproduced,
                "expected_reproduced": expect_reproduced,
                "assertions": assertions,
                "failing_assertion": failures[0] if failures else None,
                "snapshot": snapshot,
                "stale": sorted(stale),
                "warning_emitted": warning,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0 if outcome_matches else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Iterable[str]) -> int:
    parser = argparse.ArgumentParser(description="Cabin log analysis CLI")
    subparsers = parser.add_subparsers(dest="command", required=True)

    trace_parser = subparsers.add_parser("trace")
    trace_parser.add_argument("trace_id")
    trace_parser.add_argument("logs", nargs="+")
    trace_parser.set_defaults(func=command_trace)

    stats_parser = subparsers.add_parser("stats")
    stats_parser.add_argument("logs", nargs="+")
    stats_parser.add_argument("--json", action="store_true")
    stats_parser.set_defaults(func=command_stats)

    triage_parser = subparsers.add_parser("triage")
    triage_parser.add_argument("log")
    triage_parser.set_defaults(func=command_triage)

    repro_parser = subparsers.add_parser("repro")
    repro_parser.add_argument("capture")
    repro_parser.add_argument("--expect", default="")
    repro_parser.add_argument("--build-dir", default=str(ROOT / "build"))
    repro_parser.set_defaults(func=command_repro)

    args = parser.parse_args(list(argv))
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
