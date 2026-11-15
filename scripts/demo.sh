#!/usr/bin/env bash
# End-to-end demo: broker + signald + cabin services + personalization + gateway.
# Writes the measured profile-apply p95 to results/personalization.json.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_DIR="${BUILD_DIR:-build}"
PORT="${CABIN_DEMO_PORT:-18080}"
RUNTIME_DIR="$(mktemp -d /tmp/cabin-demo-XXXXXX)"
STATE_DIR="${RUNTIME_DIR}/state"
LOGS="${RUNTIME_DIR}/logs"
mkdir -p "${LOGS}" "${STATE_DIR}"

export CABIN_RUNTIME_DIR="${RUNTIME_DIR}"
export CABIN_STATE_DIR="${STATE_DIR}"
export CABIN_LOG_LEVEL="${CABIN_LOG_LEVEL:-info}"

PIDS=()
cleanup() {
  set +e
  for pid in "${PIDS[@]:-}"; do kill "${pid}" 2>/dev/null; done
  for pid in "${PIDS[@]:-}"; do wait "${pid}" 2>/dev/null; done
  rm -rf "${RUNTIME_DIR}"
}
trap cleanup EXIT

for binary in cabind signald cabin-services personalizationd cabin-gateway; do
  if [[ ! -x "${BUILD_DIR}/${binary}" ]]; then
    echo "missing ${BUILD_DIR}/${binary}; run scripts/build.sh first" >&2
    exit 1
  fi
done

start() {
  local name="$1"; shift
  "$@" >"${LOGS}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

start cabind "${BUILD_DIR}/cabind" --runtime-dir "${RUNTIME_DIR}"
sleep 0.3
start signald "${BUILD_DIR}/signald" --source replay:captures/cabin_normal.log \
  --runtime-dir "${RUNTIME_DIR}"
sleep 0.3
start cabin-services "${BUILD_DIR}/cabin-services" --runtime-dir "${RUNTIME_DIR}"
start personalizationd "${BUILD_DIR}/personalizationd" --runtime-dir "${RUNTIME_DIR}" \
  --state-dir "${STATE_DIR}"
start cabin-gateway "${BUILD_DIR}/cabin-gateway" --runtime-dir "${RUNTIME_DIR}" \
  --port "${PORT}"

ready=0
for _ in $(seq 1 100); do
  if curl -fsS "http://127.0.0.1:${PORT}/healthz" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.05
done
if [[ "${ready}" -ne 1 ]]; then
  echo "gateway did not become ready" >&2
  for log in "${LOGS}"/*.log; do
    echo "--- ${log} ---" >&2
    tail -20 "${log}" >&2
  done
  exit 1
fi

echo "== PUT profile =="
curl -sS -X PUT "http://127.0.0.1:${PORT}/v1/profiles/driver-a" \
  -H 'Content-Type: application/json' \
  -H 'X-Cabin-Trace-Id: 800000000000d001' \
  -d '{"seat_position_mm":520,"seat_recline_deg":24,"hvac_setpoint_c":20.5,"hvac_fan_level":4,"light_level":45,"light_color":"NEUTRAL"}' \
  | python3 -m json.tool

echo "== POST apply =="
curl -sS -X POST "http://127.0.0.1:${PORT}/v1/profiles/driver-a/apply" \
  -H 'X-Cabin-Trace-Id: 800000000000d002' | python3 -m json.tool

echo "== GET /v1/signals =="
curl -sS "http://127.0.0.1:${PORT}/v1/signals" \
  -H 'X-Cabin-Trace-Id: 800000000000d003' | tee "${RUNTIME_DIR}/signals.json" \
  | python3 -m json.tool

echo "== measuring profile-apply latency (200 applies) =="
python3 - "${PORT}" "${BUILD_DIR}" <<'PY'
import json
import platform
import sys
import urllib.request

port = int(sys.argv[1])
build_dir = sys.argv[2]
latencies = []
for _ in range(200):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/v1/profiles/driver-a/apply",
        method="POST",
        data=b"",
        headers={"X-Cabin-Trace-Id": "800000000000d100"},
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        body = json.loads(response.read())
    latencies.append(body["latency_us"])

latencies.sort()
def pct(p):
    index = int(round((len(latencies) - 1) * p))
    return latencies[index]

result = {
    "benchmark": "profile_apply_http",
    "host": platform.node(),
    "build_dir": build_dir,
    "iterations": len(latencies),
    "p50_us": pct(0.50),
    "p95_us": pct(0.95),
    "p99_us": pct(0.99),
    "max_us": max(latencies),
    "mean_us": round(sum(latencies) / len(latencies), 1),
}
with open("results/personalization.json", "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(result, indent=2, sort_keys=True))
PY

echo "== done =="
