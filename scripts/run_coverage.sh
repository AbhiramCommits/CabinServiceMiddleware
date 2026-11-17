#!/usr/bin/env bash
# Coverage for src/ only (generated code, tests and fuzz are excluded).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
BUILD_DIR="${COVERAGE_BUILD_DIR:-build-coverage}"
mkdir -p results

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug \
  -DCABIN_ENABLE_COVERAGE=ON -DCABIN_BUILD_TESTS=ON >/dev/null
cmake --build "${BUILD_DIR}" -j "${JOBS}" >/dev/null
# Stale .gcda files from a previous build are rejected by gcov ("Invalid
# .gcda File"); always start from a clean counter set.
find "${BUILD_DIR}" -name "*.gcda" -delete
if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${CTEST_JOBS:-2}" \
    > results/coverage_ctest.log 2>&1; then
  echo "ctest failed under coverage instrumentation; failing tests:" >&2
  grep -E "\*\*\*(Failed|Timeout|Exception)|tests failed" results/coverage_ctest.log >&2 || true
  tail -n 80 results/coverage_ctest.log >&2
  exit 1
fi

GCOVR_GCOV=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v llvm-cov >/dev/null 2>&1; then
  GCOVR_GCOV=(--gcov-executable "llvm-cov gcov")
elif [[ -x /opt/homebrew/opt/llvm/bin/llvm-cov ]]; then
  GCOVR_GCOV=(--gcov-executable "/opt/homebrew/opt/llvm/bin/llvm-cov gcov")
fi

gcovr --root . --filter 'src/' \
  "${GCOVR_GCOV[@]}" \
  --json results/coverage.json --json-pretty \
  --txt results/coverage.txt \
  "${BUILD_DIR}" >/dev/null

python3 - <<'PY'
import json

with open("results/coverage.json") as handle:
    data = json.load(handle)

line_counts = {}
function_total = 0
function_covered = 0
for entry in data.get("files", []):
    for line in entry.get("lines", []):
        key = (entry["file"], line["line_number"])
        line_counts[key] = max(line_counts.get(key, 0), int(line.get("count", 0)))
    for function in entry.get("functions", []):
        function_total += 1
        if int(function.get("execution_count", 0)) > 0:
            function_covered += 1

line_total = len(line_counts)
line_covered = sum(1 for count in line_counts.values() if count > 0)
line_percent = 100.0 * line_covered / line_total if line_total else 0.0
function_percent = (
    100.0 * function_covered / function_total if function_total else 0.0
)
data["line_total"] = line_total
data["line_covered"] = line_covered
data["line_percent"] = round(line_percent, 2)
data["function_total"] = function_total
data["function_covered"] = function_covered
data["function_percent"] = round(function_percent, 2)
with open("results/coverage.json", "w") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
with open("results/coverage.txt", "a") as handle:
    handle.write(
        f"\nTOTAL src/ line {line_covered}/{line_total} = {line_percent:.1f}%  "
        f"function {function_covered}/{function_total} = {function_percent:.1f}%\n"
    )
print(
    f"coverage: line {line_percent:.1f}% ({line_covered}/{line_total}) "
    f"function {function_percent:.1f}% ({function_covered}/{function_total}) (src/ only)"
)
PY
