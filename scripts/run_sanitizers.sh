#!/usr/bin/env bash
# ASan+UBSan over the whole suite, then a TSan pass over the concurrency-heavy
# broker/client/gateway tests. Leak detection is enabled where the platform
# supports it (Linux); Apple's ASan does not implement LeakSanitizer.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
mkdir -p results
FAIL=0

run_pass() {
  local dir="$1" sanitizer="$2" label="$3" pattern="${4:-}"
  echo "== ${label}: configuring ${dir} =="
  cmake -S . -B "${dir}" -DCMAKE_BUILD_TYPE=Debug \
    -DCABIN_ENABLE_SANITIZERS=ON -DCABIN_SANITIZER="${sanitizer}" \
    -DCABIN_BUILD_TESTS=ON >/dev/null
  cmake --build "${dir}" -j "${JOBS}" >/dev/null

  local options="halt_on_error=1"
  if [[ "$(uname -s)" == "Linux" && "${sanitizer}" == "address" ]]; then
    options="detect_leaks=1:${options}"
  fi
  echo "== ${label}: running ctest =="
  local ctest_args=(--test-dir "${dir}" --output-on-failure -j "${CTEST_JOBS:-2}")
  if [[ -n "${pattern}" ]]; then
    ctest_args+=(-R "${pattern}" -E "FiftyThousand")
  fi
  ASAN_OPTIONS="${options}" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    TSAN_OPTIONS="halt_on_error=1" ctest "${ctest_args[@]}"
}

ASAN_STATUS=clean
TSAN_STATUS=clean
if ! run_pass build-asan address "ASan+UBSan"; then
  ASAN_STATUS=failed
  FAIL=1
fi
if ! run_pass build-tsan thread "TSan" "Broker|Gateway|Personalization"; then
  TSAN_STATUS=failed
  FAIL=1
fi

cat > results/sanitizers.json <<JSON
{
  "asan_ubsan": {
    "status": "${ASAN_STATUS}",
    "build_dir": "build-asan",
    "platform": "$(uname -s)",
    "options": "halt_on_error=1,detect_leaks=$([[ "$(uname -s)" == "Linux" ]] && echo 1 || echo unsupported)"
  },
  "tsan": {
    "status": "${TSAN_STATUS}",
    "build_dir": "build-tsan",
    "tests": "Broker|Gateway|Personalization (50k soak excluded)",
    "options": "halt_on_error=1"
  }
}
JSON

if [[ "${FAIL}" -ne 0 ]]; then
  echo "sanitizers: FAILED" >&2
  exit 1
fi
echo "sanitizers: clean"
