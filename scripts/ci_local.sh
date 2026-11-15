#!/usr/bin/env bash
# Local mirror of CI jobs 1-6. Grows with each project step. Every gate prints
# exactly what it ran so a failure can be reproduced without GitHub.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_DIR="${BUILD_DIR:-build}"
FAIL=0
step() { echo; echo "── $* ─────────────────────────────────────────────"; }
check() {
  local name="$1"; shift
  if "$@"; then
    echo "PASS: ${name}"
  else
    echo "FAIL: ${name}"
    FAIL=1
  fi
}

step "1/6 format"
if command -v clang-format >/dev/null 2>&1; then
  files=$(find include src tests tools idl generated fuzz bench -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null || true)
  if [[ -n "${files}" ]]; then
    check "clang-format --dry-run --Werror" bash -c "clang-format --dry-run --Werror ${files//$'\n'/ }"
  fi
else
  echo "SKIP: clang-format not installed"
fi

step "2/6 code generation drift"
if [[ -f tools/idlc.py ]]; then
  check "idlc --check" python3 tools/idlc.py --check
fi
if [[ -f tools/gen_tests.py ]]; then
  check "gen_tests --check" python3 tools/gen_tests.py --check
fi

step "3/6 build + test"
check "build" ./scripts/build.sh
check "ctest" env BUILD_DIR="${BUILD_DIR}" ./scripts/test.sh

step "4/6 sanitizers"
if [[ -x scripts/run_sanitizers.sh ]]; then
  check "sanitizers" ./scripts/run_sanitizers.sh
else
  echo "SKIP: run_sanitizers.sh not present yet"
fi

step "5/6 coverage + thresholds"
if [[ -x scripts/run_coverage.sh ]]; then
  check "coverage" ./scripts/run_coverage.sh
  if [[ -f tools/check_thresholds.py ]]; then
    check "thresholds" python3 tools/check_thresholds.py
  fi
else
  echo "SKIP: run_coverage.sh not present yet"
fi

step "6/6 fuzz smoke"
if [[ -x scripts/run_fuzz_smoke.sh ]]; then
  check "fuzz smoke" ./scripts/run_fuzz_smoke.sh
else
  echo "SKIP: run_fuzz_smoke.sh not present yet"
fi

echo
if [[ "${FAIL}" -ne 0 ]]; then
  echo "ci_local: FAILED"
  exit 1
fi
echo "ci_local: OK"
