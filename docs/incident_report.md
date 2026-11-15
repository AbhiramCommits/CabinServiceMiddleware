# Incident report INC-2026-014

**Title:** HVAC setpoint occasionally rejected after ignition cycle
**Severity:** High (cabin comfort control loss, no unsafe actuation observed)
**Component:** `signald` / `SignalCodec`, surfaced by `HvacService` and
`PersonalizationService`
**Affected build:** v1.2.0, Linux x86_64 fleet validation, CAN replay from a
vehicle capture
**Status:** Fixed, regression pinned, reproduction now passes

## Report

The validation team observed that after an ignition OFF → ON cycle, a stored
driver profile was occasionally not applied and the HVAC state query returned
`400 OUT_OF_RANGE`. The failure was intermittent: it only appeared in sessions
whose capture contained a corrupted HVAC frame. The reported symptom was
"HVAC setpoint rejected after ignition cycle".

Expected: a corrupted sensor sample is clamped, logged, and the last valid
setpoint remains usable. Actual (before the fix): a non-finite IEEE-754 setpoint
survived decoding, entered the signal cache, and caused the generated
`HvacState`/`SignalSnapshot` encoder to reject the value with `OUT_OF_RANGE`.

## Reproduction

The fault capture contains a 40 °C setpoint (finite, out of range), a NaN
setpoint at 0.7 s (`130#0000c07f03`), a later valid 22 °C frame, and a seat frame
that is not refreshed for 2.4 s.

```console
$ python3 tools/logtool.py repro captures/cabin_fault.log
symptom: HVAC setpoint rejected after ignition cycle (non-finite CAN sample)
capture: captures/cabin_fault.log
snapshot: {"cabin_light_color": "NEUTRAL", "cabin_light_level": 80, "door_state": "AJAR", "hvac_fan_level": 4, "hvac_setpoint_c": 22.0, "ignition_state": "ON", "seat_position_mm": 420, "seat_recline_deg": 18, "timestamp_us": 1700000002400000}
assertions:
  [PASS] seat_position_mm == 420 (actual=420)
  [PASS] seat_recline_deg == 18 (actual=18)
  [PASS] hvac_setpoint_c == 22.0 (actual=22.0)
  [PASS] hvac_fan_level == 4 (actual=4)
  [PASS] cabin_light_level == 80 (actual=80)
  [PASS] cabin_light_color == NEUTRAL (actual=NEUTRAL)
  [PASS] door_state == AJAR (actual=AJAR)
  [PASS] ignition_state == ON (actual=ON)
  [PASS] seat_position_mm is stale (actual=True)
  [PASS] seat_recline_deg is stale (actual=True)
  [PASS] warning signal_out_of_range emitted (actual=True)
  [PASS] hvac_setpoint_c is finite and within 16.0..30.0 after clamping (actual=22.0)
failing assertion: none (guard held)
symptom reproduced: no (expected no)
```

`logtool repro` starts a fresh `cabind`, replays the capture through a fresh
`signald`, discovers `SignalService` over the real broker protocol, and queries
the final snapshot. The reproduction therefore exercises the guard, not a mock.

The command exits 0 because the outcome matches
`captures/cabin_fault.expect.json` (`expect_symptom_reproduced: false`).

## Trace timeline

A normal ignition/apply session was captured with

```sh
CABIN_LOG_LEVEL=trace CABIN_LOG_FILE=captures/incident_session.jsonl ...
curl -X PUT    -H 'X-Cabin-Trace-Id: 800000000000e001' ...
curl -X POST   -H 'X-Cabin-Trace-Id: 800000000000e001' .../apply
```

`captures/incident_session.jsonl` is the trace-filtered excerpt of that run (54
records, one trace id), kept in the repository so the timeline is reproducible.
`logtool trace` reconstructs the ordered timeline across eight services:

```console
$ python3 tools/logtool.py trace 800000000000e001 captures/incident_session.jsonl
trace 800000000000e001: 54 records across 8 services
      t+us  service            event              span name                  dur_us
         0  gateway            span_begin         http_request
        91  client             span_begin         rpc_call
       232  PersonalizationService span_begin         handle_request
       733  personalization    profile_stored
       754  PersonalizationService span_end           handle_request              521.0
       801  client             span_end           rpc_call                    128.0
       848  gateway            http_request       http_request
       857  gateway            span_end           http_request                858.0
...
     11521  PersonalizationService span_begin         handle_request
     11547  personalization    span_begin         ApplyProfile
     11558  client             span_begin         rpc_call
     11652  SeatService        span_begin         handle_request
     11732  client             span_begin         rpc_call
     11836  SignalService      span_begin         handle_request
     11922  SignalService      span_end           handle_request               86.0
     11963  client             span_end           rpc_call                    229.0
     12147  SeatService        span_end           handle_request              495.0
     12239  HvacService        span_begin         handle_request
     12308  SignalService      span_begin         handle_request
     12336  SignalService      span_end           handle_request               28.0
     12523  HvacService        span_end           handle_request              285.0
     12604  LightingService    span_begin         handle_request
     13434  gateway            span_end           http_request               2060.0

slowest hop: PersonalizationService::handle_request 521.0 us
```

The apply trace is the path the failing value takes: gateway → personalization
→ Seat/HVAC/Lighting, with an inner SignalService hop per actuator. The long
`rpc_call` (1995 µs) is the personalization worker waiting behind the earlier
seat apply; the HVAC handler itself is 285 µs. The rejection was not a timeout
or routing failure: it happened when a non-finite value reached the generated
encoder at the end of this chain.

The slowest leaf in the capture is the profile `PutProfile` handler at 521 µs,
dominated by the atomic `profiles.json` write (temp file, `fsync`, rename). That
is expected, and it is why profile writes are not on the signal path.

## Root cause

`SignalCodec::decode_bits` clamped with ordered comparisons:

```cpp
if (value < meta.min) {
  value = meta.min;
} else if (value > meta.max) {
  value = meta.max;
}
```

Every comparison against NaN is false, so a NaN setpoint passed through with
`SignalQuality::kOk`. `SignalCache` stored it, and the next `SignalSnapshot`
encode failed because the generated validator correctly requires
`std::isfinite`. That error surfaced as `OUT_OF_RANGE` on the HVAC state path
after the post-ignition profile apply.

## Fix

Commit `2cf3cc0` (`test: add AI-assisted test generation, fuzz targets,
sanitizer and coverage gates with empirical thresholds`) replaced the clamp
with an explicit non-finite branch:

```cpp
if (std::isnan(value)) {
  value = meta.min;                  // safe fallback
  quality = SignalQuality::kInvalid; // never mistaken for a live measurement
} else if (!std::isfinite(value)) {
  value = value > 0.0 ? meta.max : meta.min;
  quality = SignalQuality::kClamped;
} else if (value < meta.min) {
  ...
```

The `signal_out_of_range` warning is emitted for all three cases, so a fleet
trace still shows the corrupted frame.

## Regression tests

The generated, pinned suite in `tests/generated/` exposed the defect on its
first run: the NaN signal case failed with `kOk` instead of `kInvalid`. After
the fix:

* `signal:hvac_setpoint_c:nan` — expects `kInvalid` with the safe fallback
  value 16.0.
* `signal:hvac_setpoint_c:plus_inf` / `minus_inf` — expect clamped max/min.
* `signals:hvac_setpoint_c:above_max` and the finite 40 °C fault frame — expect
  clamped values plus a `signal_out_of_range` record.
* `Signals.FaultCaptureSurfacesClampAndStaleSeat` asserts the fault capture
  produces the warning and marks the dropped seat signal stale.

`python3 tools/gen_tests.py --check` pins these cases; CI fails on drift, so the
regression cannot be edited away without review.

## Verification

```console
$ ctest --test-dir build -R 'Generated|Signals' --output-on-failure
100% tests passed
```

The full-suite ASan+UBSan and TSan runs are clean (`results/sanitizers.json`),
and `tools/check_thresholds.py` passes. The fault capture now leaves the HVAC
setpoint at the later valid 22.0 °C, marks the seat frame stale, and never
admits a non-finite value into the cache.

## Follow-ups

* Add per-signal E2E counters; a CRC over the broker frame cannot detect a
  corrupted CAN payload that was already accepted by the driver.
* Emit the `kInvalid` quality in `SignalSnapshot` so consumers can distinguish
  a clamped sample from a fresh measurement (currently the value is safe but
  the snapshot carries no quality).
* Add a watchdog assertion on `signald` that refuses to publish if the cache
  contains a non-finite value (defence in depth).
