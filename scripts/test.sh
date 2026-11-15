#!/usr/bin/env bash
# Run the test suite and validate a real log line as JSON with Python's stdlib.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_DIR="${BUILD_DIR:-build}"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: ${BUILD_DIR} does not exist; run scripts/build.sh first" >&2
  exit 1
fi

ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${CTEST_JOBS:-4}"

PROBE="${BUILD_DIR}/tests/cabin_log_probe"
if [[ -x "${PROBE}" ]]; then
  echo "== validating emitted log line as JSON =="
  CABIN_LOG_LEVEL=info "${PROBE}" 2>&1 >/dev/null | python3 -c '
import json, sys
lines = [line for line in sys.stdin if line.strip()]
assert lines, "probe emitted no log line"
for line in lines:
    obj = json.loads(line)
    for key in ("ts_us", "level", "service", "trace_id", "span", "event"):
        assert key in obj, f"missing key {key}: {obj}"
assert any(obj.get("event") == "sample" for obj in map(json.loads, lines)), "no sample event"
print(f"validated {len(lines)} JSON log line(s)")
'
fi
