# CabinServiceMiddleware

CabinServiceMiddleware is a service-oriented C++17 middleware layer for a
Software-Defined Vehicle cabin domain. It models the problem a real SDV cabin
faces: a handful of ECUs produce safety-relevant signals over CAN, several
independent service processes own actuation, a personalization service persists
driver intent, and a vehicle-internal REST surface must expose all of it without
forking the API. The repository gives every one of those pieces a real process,
a real Unix-domain-socket protocol (SOME/IP-style discovery, pub/sub and
request/response), and an IDL that is the single source of truth for types,
ranges, serialization and the generated client/server surface.

The middleware is deliberately inspectable. `tools/idlc.py` turns
`idl/cabin.yaml` into C++17 headers and serdes code plus a machine-readable
schema; `cabind` brokers service discovery and messages; `signald` turns CAN
frames (replayed files on any OS, SocketCAN on Linux) into typed, range-checked
signals; Seat, HVAC and Lighting services actuate; PersonalizationService stores
versioned profiles and applies them on ignition; `cabin-gateway` transcodes
HTTP/1.1 to the same IDL calls and propagates a trace id end to end. Every wire
frame carries a CRC and an IDL major/minor version, and the whole thing is
covered by unit, integration, generated, fuzz and sanitizer gates.

## Architecture

```
  CAN ECUs / candump capture                 REST client (curl, vehicle HMI)
            │                                          │ HTTP/1.1
            ▼                                          ▼
  ┌───────────────────┐                      ┌──────────────────────┐
  │ signald           │                      │ cabin-gateway        │
  │ ICanSource        │                      │ /healthz             │
  │  replay | SocketCAN│                     │ /v1/signals          │
  │ SignalCodec       │                      │ /v1/profiles/{id}    │
  │ SignalCache       │                      │ .../apply            │
  └────────┬──────────┘                      └──────────┬───────────┘
           │ NOTIFY SignalChanged                       │ IDL RPC
           ▼                                            ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │ cabind                                                           │
  │  OFFER/FIND/SERVICE_AVAILABLE, SUBSCRIBE/NOTIFY, REQUEST/RESPONSE│
  │  per-client bounded queues, CRC, major/minor negotiation         │
  └───▲──────────────▲──────────────▲──────────────▲─────────────────┘
      │              │              │              │
  ┌───┴────┐    ┌────┴────┐    ┌────┴─────┐   ┌────┴─────────────────┐
  │ Seat   │    │ HVAC    │    │ Lighting │   │ Personalization      │
  │Service │    │Service  │    │ Service  │   │ Get/Put/ApplyProfile │
  └────────┘    └─────────┘    └──────────┘   │ profiles.json (v1.2) │
                                              └──────────────────────┘
```

**IDL → codegen → wire.** `idl/cabin.yaml` (JSON syntax, a valid YAML 1.2
subset; no PyYAML dependency) declares enums, typed signals with units, ranges
and CAN mappings, message structs and five services. `tools/idlc.py` emits
`generated/cabin_version.hpp`, `cabin_types.hpp`, `cabin_serdes.{hpp,cpp}`,
`cabin_services.hpp`, `cabin_can_map.{hpp,cpp}`, `cabin_schema.json` and
`docs/api_v1.md`. The frame format is
`magic(4) | idl_major | idl_minor | msg_type | flags | service_id | method_id |
seq | trace_id | payload_len | payload | crc32`; `cabin/frame.hpp` rejects bad
magic, bad CRC, truncation, and a mismatched **major** (`VERSION_MISMATCH`),
while accepting a newer **minor** for forward compatibility.

**Discovery, versioning, tracing.** No component hard-codes another's endpoint:
providers `OFFER_SERVICE`, consumers `FIND_SERVICE` and receive
`SERVICE_AVAILABLE/UNAVAILABLE`. Additive API fields carry `since_minor` and are
last in their struct, so an older decoder ignores them and a newer decoder
defaults them (`Profile.preferred_language` is the worked example). A 64-bit
trace id lives in every frame and in thread-local context; `X-Cabin-Trace-Id`
enters at the gateway and `tools/logtool.py trace` reconstructs the cross-process
timeline, including per-span durations.

## How to run

**Prerequisites.** CMake ≥ 3.20, a C++17 compiler, Python 3 (stdlib only),
`curl` for the demo, and network access on the first configure for GoogleTest
v1.14.0 via FetchContent. Linux additionally supports SocketCAN (`vcan`) and
systemd. Everything except SocketCAN and systemd builds and runs on macOS.

```sh
./scripts/build.sh        # build/ (Debug) and build-release/ (Release)
./scripts/test.sh         # ctest + JSON log-line validation
./scripts/demo.sh         # starts the full stack, PUTs a profile, applies it
```

The demo starts `cabind`, `signald`, `cabin-services`, `personalizationd` and
`cabin-gateway` in a temporary runtime directory, then:

```sh
curl -X PUT  http://127.0.0.1:18080/v1/profiles/driver-a -d '{...}'
curl -X POST http://127.0.0.1:18080/v1/profiles/driver-a/apply
curl        http://127.0.0.1:18080/v1/signals
```

It prints the resulting signal state and writes the measured profile-apply
latency to `results/personalization.json`.

**SocketCAN (Linux).**

```sh
sudo ./scripts/setup_vcan.sh vcan0
./build/signald --source socketcan:vcan0 --runtime-dir /tmp/cabin
ctest --test-dir build -R SocketCan --output-on-failure
```

The replay source is the portable path and is what every test uses:

```sh
./build/signald --source replay:captures/cabin_normal.log --once
./build/signald --source replay:captures/cabin_normal.log:realtime --runtime-dir /tmp/cabin
```

**systemd.** `systemd/cabind.service` and `systemd/cabin-gateway.service` are
hardened units (`ProtectSystem=strict`, syscall and address-family filters,
`Restart=on-failure`). See `docs/deployment.md` for the process topology and
install commands.

**Tools.**

```sh
python3 tools/idlc.py                     # regenerate C++/schema/docs
python3 tools/idlc.py --check             # fail on codegen drift
python3 tools/gen_tests.py                # deterministic generated suites
python3 tools/gen_tests.py --llm          # optional adversarial LLM cases
python3 tools/gen_tests.py --check        # fail on test/corpus drift
python3 tools/logtool.py trace <trace_id> captures/incident_session.jsonl
python3 tools/logtool.py stats captures/incident_session.jsonl [--json]
python3 tools/logtool.py triage results/ctest.log
python3 tools/logtool.py repro captures/cabin_fault.log
python3 tools/check_thresholds.py
./scripts/run_sanitizers.sh
./scripts/run_coverage.sh
./scripts/run_fuzz_smoke.sh
./scripts/ci_local.sh                     # local mirror of CI jobs 1-6
```

### Commands that produce each number

| Number | Command | Output |
| --- | --- | --- |
| IPC latency / throughput | `./build-release/bench_ipc --messages 200000 --payload-bytes 64 --subscribers 2` | `results/ipc_latency.json` |
| Profile-apply p95 | `./scripts/demo.sh` | `results/personalization.json` |
| Coverage | `./scripts/run_coverage.sh` | `results/coverage.json`, `results/coverage.txt` |
| Generated case counts | `python3 tools/gen_tests.py` | `results/testgen.json`, `tests/generated/PROVENANCE.md` |
| Sanitizer status | `./scripts/run_sanitizers.sh` | `results/sanitizers.json` |
| Fuzz executions | `./scripts/run_fuzz_smoke.sh` | `results/fuzz.json` |
| Incident outcome | `python3 tools/logtool.py repro captures/cabin_fault.log` | `results/repro.json` |
| Test count | `ctest --test-dir build -N` | terminal |

## Results (measured on this build)

**IPC publish → receive latency.** `bench_ipc` Release build, 200,000 messages,
64-byte payload, 2 subscribers, 400,000 delivered samples, 0 dropped. Host
`Abhirams-MacBook-Air.local` (Apple silicon), compiler AppleClang 21.0.0.21000401
(`-O3` Release).

| p50 | p95 | p99 | max | throughput |
| --- | --- | --- | --- | --- |
| 584 µs | 730 µs | 813 µs | 3,836 µs | 170,890 msg/s |

**Profile apply (HTTP → Personalization → Seat + HVAC + Lighting → SignalService).**
Debug build (`build/`), 200 applies, measured by the service around the three
actuation RPCs: p50 **606 µs**, p95 **1,353 µs**, p99 **2,208 µs**, max
**4,327 µs**, mean **719.1 µs**.

**Coverage (src/ only, generated/tests/fuzz excluded).** Line **66.57%**
(2,409/3,619), function **75.26%** (216/287), from `results/coverage.json`.
Coverage includes the daemon `main()` files that only the demo starts; that is
why it is not higher.

**Generated tests.** 286 pinned cases: **286 schema origin**, **0 LLM origin**
(the LLM stage was not invoked in this generation; without
`ANTHROPIC_API_KEY` `--llm` is a no-op fallback). Expected valid **161**,
expected rejected **125**, LLM proposed/accepted/rejected **0/0/0**. The ctest
suite has **305 tests**: 253 generated parameterized cases plus 52 hand-written
unit/integration tests. On Linux the compile-guarded SocketCAN integration adds
one more (306).

**Sanitizers.** ASan+UBSan whole suite: **clean**. TSan over broker, client,
gateway and personalization tests: **clean** (50k soak excluded from the TSan
pass because it is not a race test). Locally on macOS, ASan reports
`detect_leaks` as unsupported; on Linux CI the script sets `detect_leaks=1`.

**Fuzzing.** Local fallback (AppleClang, libFuzzer unavailable in this toolchain):
`fuzz_frame` **200,033 executions**, `fuzz_signal_decode` **200,036
executions**, both clean. Corpus: **69 files** generated from the pinned cases
(33 frame seeds, 36 signal seeds). On Linux+Clang CI runs real libFuzzer for 10
seconds per target with the same corpus.

**Incident reproduction.** `logtool repro captures/cabin_fault.log` starts a
fresh broker + signald, replays the capture and queries `SignalService` over the
real protocol: **symptom not reproduced (expected no)**, **no failing
assertion**, all 12 safety assertions pass; the NaN setpoint is clamped and
logged, the dropped seat signal is flagged stale, and the final HVAC setpoint is
the later valid 22.0 °C. See `docs/incident_report.md`.

**Thresholds.** `python3 tools/check_thresholds.py` passes against
`thresholds.json`: line coverage floor 60%, IPC p99 ≤ 2,600 µs, profile-apply
p95 ≤ 2,500 µs, sanitizer status clean. The thresholds and their derivation are
recorded in the file; they are empirical, not aspirational.

## Safety & standards

`docs/safety.md` maps each cabin function to a requirement ID and an
ISO 26262-informed ASIL rationale (seat movement is treated as ASIL B because
unintended movement can crush an occupant; HVAC is ASIL A/QM; lighting is QM),
and lists the freedom-from-interference measures: process isolation, bounded
queues with drop-oldest events, request timeouts, fail-closed version checks,
CRC integrity, stale-signal detection, a watchdog warning on a silent SocketCAN
source, and an allocation-free decode/cache path. It also states which
MISRA C++ / AUTOSAR C++14 rules the code follows and where it deliberately
deviates (STL containers, startup allocation, exceptions at the HTTP boundary,
IEEE-754 for the HVAC setpoint).

## Limitations and what production would change

* **Transport is a stand-in.** The broker is a Unix-socket protocol deliberately
  shaped like SOME/IP; production would use vSomeIP (or a certified
  service-oriented stack). `IRpcChannel` is the seam, and
  `docs/gateway_mapping.md` gives the exact service/method/event and gRPC
  mappings.
* **No DBC ingestion.** CAN mappings are authored in the IDL. A production
  toolchain would import DBC/ARXML and generate the IDL mapping.
* **No authenticated CAN.** CRC-32 detects accidental corruption, not a
  malicious bus participant; SecOC/E2E protection is missing.
* **ASIL is a rationale, not a certificate.** There is no redundant signal
  path, no seat obstacle detection, no lock-step monitoring and no formal
  verification.
* **Single broker is a SPOF.** Discovery and routing would need redundancy or a
  transport with built-in redundancy.
* **The LLM test stage is optional and not keyed in CI.** It is validated
  against the schema and deduplicated, but the committed generation is the
  deterministic 286-case set.
* **Coverage excludes main() behaviour exercised only by the demo**, and the
  benchmark host is a laptop; the CI matrix is what makes the numbers
  reproducible across compilers.

## Repo map

```
idl/cabin.yaml               IDL: enums, signals, CAN map, structs, services
generated/                   pinned idlc output (types, serdes, services, schema)
include/cabin/               Result/Bytes/log/trace/frame, broker/client,
                             signals (CAN, codec, cache, services),
                             personalization, gateway, JSON
src/core/                    logging, tracing, frame/CRC, JSON, runtime paths,
                             client/host transport
src/broker/                  cabind poll loop, discovery, routing, pub/sub
src/signals/                 replay + SocketCAN sources, codec, cache, signald,
                             seat/HVAC/lighting services
src/personalization/         profile store + versioned service
src/gateway/                 HTTP/1.1 server + REST→IDL transcoder
tests/                       hand-written unit/integration tests
tests/generated/             pinned cases.json, generated suites, PROVENANCE
fuzz/                        libFuzzer targets + pinned corpus
bench/                       bench_ipc
tools/                       idlc.py, gen_tests.py, logtool.py,
                             check_thresholds.py
scripts/                     build/test/format/ci_local, demo, sanitizers,
                             coverage, fuzz smoke, vcan setup
captures/                    candump captures + incident trace + expectations
results/                     real measured JSON outputs
systemd/                     hardened cabind/gateway units
docs/                        deployment, API versioning, gateway mapping,
                             safety, incident report, generated API reference
```
