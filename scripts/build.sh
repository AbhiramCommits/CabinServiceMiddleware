#!/usr/bin/env bash
# Build both Debug and Release trees. Debug is the default `build/` directory so
# `./build/<tool>` works as documented; `build-release/` is what benchmarks use.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
GENERATOR="${CMAKE_GENERATOR:-}"

configure_and_build() {
  local dir="$1" type="$2"
  echo "== configuring ${dir} (${type}) =="
  if [[ -n "${GENERATOR}" ]]; then
    cmake -S . -B "${dir}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE="${type}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  else
    cmake -S . -B "${dir}" -DCMAKE_BUILD_TYPE="${type}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  fi
  cmake --build "${dir}" -j "${JOBS}"
}

configure_and_build build Debug
configure_and_build build-release Release

echo "builds complete: build/ (Debug), build-release/ (Release)"
