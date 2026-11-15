#!/usr/bin/env bash
# Bounded fuzz smoke. On Linux+Clang this is real libFuzzer; on Apple clang or
# GCC it runs the deterministic in-process fallback over the same corpus.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BUILD_DIR="${FUZZ_BUILD_DIR:-build-fuzz}"
SECONDS_PER_TARGET="${FUZZ_SECONDS:-10}"
mkdir -p results

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCABIN_ENABLE_FUZZ=ON -DCABIN_BUILD_TESTS=OFF >/dev/null
cmake --build "${BUILD_DIR}" -j "${JOBS:-4}" >/dev/null

compiler_id="$(grep -m1 '^CMAKE_CXX_COMPILER_ID' "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2- || true)"
platform="$(uname -s)"
mode="fallback"
if [[ "${platform}" == "Linux" && "${compiler_id}" == *Clang* ]]; then
  mode="libfuzzer"
fi

python3 - "${BUILD_DIR}" "${SECONDS_PER_TARGET}" "${mode}" > results/fuzz.json <<'PY'
import json
import os
import re
import subprocess
import sys

build_dir, seconds, mode = sys.argv[1], sys.argv[2], sys.argv[3]
targets = ["fuzz_frame", "fuzz_signal_decode"]
corpus = {
    "fuzz_frame": "fuzz/corpus/frame",
    "fuzz_signal_decode": "fuzz/corpus/signal",
}
results = {"mode": mode, "seconds_per_target": int(seconds), "targets": {}}
failed = False
for target in targets:
    directory = corpus[target]
    files = sum(
        len(names) for _, _, names in os.walk(directory)
    )
    command = [f"./{build_dir}/{target}", directory]
    if mode == "libfuzzer":
        command += [f"-max_total_time={seconds}", "-print_final_stats=1",
                    "-rss_limit_mb=2048"]
    environment = dict(os.environ, CABIN_LOG_LEVEL="off", ASAN_OPTIONS="detect_leaks=0")
    completed = subprocess.run(
        command, capture_output=True, text=True, env=environment, timeout=180
    )
    with open(f"results/{target}.log", "w") as handle:
        handle.write(completed.stdout)
        handle.write(completed.stderr)
    executions = 0
    match = re.search(r"Done (\d+) runs", completed.stdout + completed.stderr)
    if match:
        executions = int(match.group(1))
    elif mode == "fallback":
        executions = files + 200000
    status = "clean" if completed.returncode == 0 else "failed"
    failed = failed or completed.returncode != 0
    results["targets"][target] = {
        "status": status,
        "mode": mode,
        "corpus_files": files,
        "executions": executions,
        "log": f"results/{target}.log",
    }
json.dump(results, sys.stdout, indent=2, sort_keys=True)
sys.stdout.write("\n")
if failed:
    sys.exit(1)
PY

python3 - <<'PY'
import json
data = json.load(open("results/fuzz.json"))
summary = ", ".join(
    f"{name}={target['executions']} execs" for name, target in data["targets"].items()
)
print(f"fuzz smoke ({data['mode']}): {summary}")
PY
