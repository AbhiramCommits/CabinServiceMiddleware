#!/usr/bin/env python3
"""Fail the build when measured results violate the empirical thresholds.

Thresholds live in thresholds.json and are intentionally derived from real
measurements (see the _comment field). Missing IPC/personalization/coverage
results are failures: a gate that silently skips itself is not a gate.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"


def load(name: str):
    path = RESULTS / name
    if not path.exists():
        return None
    try:
        with path.open() as handle:
            return json.load(handle)
    except json.JSONDecodeError as error:
        print(f"check_thresholds: cannot parse {path}: {error}", file=sys.stderr)
        return None


def main() -> int:
    thresholds = json.loads((ROOT / "thresholds.json").read_text())
    failures = []

    ipc = load("ipc_latency.json")
    if ipc is None:
        failures.append("results/ipc_latency.json is missing (run bench_ipc)")
    else:
        p99 = float(ipc["p99_us"])
        limit = float(thresholds["max_ipc_p99_us"])
        print(
            f"IPC publish p99: {p99:.1f} us (limit {limit:.1f} us, "
            f"{ipc.get('build_type', '?')} {ipc.get('compiler', '?')})"
        )
        if p99 > limit:
            failures.append(f"IPC p99 {p99:.1f} us exceeds {limit:.1f} us")

    profile = load("personalization.json")
    if profile is None:
        failures.append(
            "results/personalization.json is missing (run scripts/demo.sh)"
        )
    else:
        p95 = float(profile["p95_us"])
        limit = float(thresholds["max_profile_apply_p95_us"])
        print(f"Profile apply p95: {p95:.1f} us (limit {limit:.1f} us)")
        if p95 > limit:
            failures.append(f"profile p95 {p95:.1f} us exceeds {limit:.1f} us")

    coverage = load("coverage.json")
    if coverage is None:
        failures.append("results/coverage.json is missing (run run_coverage.sh)")
    else:
        line = float(coverage["line_percent"])
        function = float(coverage.get("function_percent", 0.0))
        limit = float(thresholds["min_line_coverage_percent"])
        print(
            f"src/ line coverage: {line:.2f}% (floor {limit:.2f}%), "
            f"function coverage {function:.2f}%"
        )
        if line < limit:
            failures.append(f"line coverage {line:.2f}% below {limit:.2f}%")

    sanitizers = load("sanitizers.json")
    if sanitizers is None:
        print("sanitizer status: not present in results/ (CI sanitizer job gates)")
    else:
        for name, entry in sanitizers.items():
            status = entry.get("status", "unknown")
            print(f"sanitizer {name}: {status}")
            if thresholds.get("fail_on_sanitizer_report") and status != "clean":
                failures.append(f"sanitizer {name} status is {status}")

    if failures:
        print("\nTHRESHOLD FAILURES:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("\nall thresholds satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
