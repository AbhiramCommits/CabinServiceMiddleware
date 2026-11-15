#!/usr/bin/env bash
# Format every hand-written C++ source. Generated files are formatted by the
# generator itself and are checked for drift in CI, not rewritten here.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

files=$(find include src tests bench fuzz -type f \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null || true)

if [[ -z "${files}" ]]; then
  echo "no C++ sources found"
  exit 0
fi

clang-format -i ${files}
echo "formatted $(wc -w <<<"${files}") files"
