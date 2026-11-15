# Safety & standards notes (ISO 26262-informed)

This document is an engineering rationale, not a certified safety case. The
project is a middleware prototype; a production ECU would add verified
end-to-end protection, a certified RTOS, and a full ISO 26262 lifecycle.

## Requirements traceability

| ID | Requirement | ASIL rationale | Implementation | Test |
| --- | --- | --- | --- | --- |
| CAB-SEAT-001 | Seat position/recline only accepted in 0..800 mm and 0..60 deg | B — unintended seat movement can crush an occupant | generated range validation, codec clamp, `SeatService` | `Serdes.RejectsEveryNumericRangeViolation`, `Signals.BoundaryMathAndStrictEncoding` |
| CAB-SEAT-002 | Profile apply moves the seat before HVAC/lighting | B — the highest-hazard actuation must complete first and be observable | fixed order in `PersonalizationService::apply_profile_internal` | `Personalization.ApplyProfileCallsSeatThenHvacThenLighting` |
| CAB-HVAC-001 | Setpoint 16.0..30.0 C, fan 0..7 | A/QM — discomfort, window fogging at extremes | generated ranges + codec clamp + `HvacService` | `Signals.OutOfRangeFrameIsClampedNotCrashed`, `Signals.FaultCaptureSurfacesClampAndStaleSeat` |
| CAB-HVAC-002 | Out-of-range CAN values are surfaced, never crash signald | A — loss of cabin control | clamp + `signal_out_of_range` log + quality flag | `Signals.FaultCaptureSurfacesClampAndStaleSeat` |
| CAB-LIGHT-001 | Light level 0..100, colour is a valid enum | QM — visibility/comfort, no direct hazard | generated enum/range validation | `Serdes.RejectsInvalidEnumsAndOversizedStrings` |
| CAB-CAN-001 | A dropped ECU frame is detectable as stale | A — stale seat state must not be treated as current | `SignalCache::is_stale`, 1 s max age | `Signals.CacheDetectsChangeAndStaleness`, `Signals.FaultCaptureSurfacesClampAndStaleSeat` |
| CAB-PERS-001 | Corrupt persisted profiles are reported, never silently replaced | QM — a bad profile can command seat movement, contributing to CAB-SEAT-001 | atomic write, validated load | `Personalization.ProfileStorePersistsAcrossRestart` |
| CAB-PERS-002 | Older clients remain compatible with additive v1.2 fields | QM — update skew during service | `since_minor` + trailing-tolerance decode | `Serdes.AdditiveV12FieldDefaultsWhenAbsentAndTrailingIsIgnored` |
| CAB-COMM-001 | Version-mismatched peers fail closed | B — an older peer must not misinterpret actuation bytes | frame major check, `VERSION_MISMATCH` | `Frame.VersionNegotiation` |
| CAB-COMM-002 | Corrupted frames never reach a service | B — same as above | CRC-32 over header+payload | `Frame.RejectsBadMagicCrcAndTruncation` |

## Hazards and ASIL

**Seat movement is the highest-risk cabin function** in this set. A wrong
position or recline command can trap or crush an occupant, so seat actuation is
treated as ASIL B for this prototype (a real seat ECU with obstacle detection
would drive a higher classification). The design therefore:

* validates both position and recline at the generated API boundary;
* clamps out-of-range CAN input and records a quality flag rather than
  propagating a bad value;
* applies the seat first during profile application, so a partial apply leaves
  the most hazardous actuator in a known, requested state and can be retried;
* never moves the seat on ignition unless a profile has been explicitly stored
  and activated.

**HVAC** is ASIL A/QM: a bad setpoint is a comfort/fogging issue, not an
immediate crush hazard. It still gets full range validation and clamping because
out-of-range values are a leading indicator of a broken sensor.

**Lighting** is QM. It is validated for type safety (enum and 0..100) but does
not drive a safety mechanism.

**Personalization** is QM on its own, but because it can command the seat it is
treated as a contributor to CAB-SEAT-001: the profile is validated before every
apply, and the persisted file is validated on load.

## Freedom from interference

* **Process isolation.** Broker, signal daemon, each consumer service group,
  personalization and the gateway are separate OS processes. A crash in the
  gateway cannot corrupt the signal cache.
* **Bounded queues.** The broker caps each client's send queue and drops the
  oldest *event* under pressure (`queue_overflow`), never growing without bound.
  Request/response traffic is preserved; a truly stuck peer is disconnected.
* **Bounded event queues** in the client (16k events) with the same drop-oldest,
  rate-limited logging policy.
* **No dynamic allocation in the decode/cache hot path.** `SignalCodec::decode_into`
  writes into a fixed `std::array` owned by `SignalProcessor`; the changed-signal
  list is reserved once. A changed snapshot is encoded (allocating) only when a
  value actually changes or the keep-alive age expires.
* **Bounded time.** Every RPC has a timeout; `ServiceClient::call` fails with
  `TIMEOUT` and removes the pending entry. Socket reader threads poll with a
  100 ms shutdown tick.
* **Watchdog / staleness.** `SignalCache` marks a signal stale after 1 s without
  a refresh, and `signald` logs `signal_watchdog_stall` when a live SocketCAN
  source produces no frame for 5 s. systemd uses `Restart=on-failure`.
* **Fail closed on version skew.** A different IDL major is rejected before the
  payload is interpreted.
* **Integrity.** CRC-32 covers header and payload. This detects accidental
  corruption, not a malicious actor; production would add SecOC/E2E protection.
* **No exceptions in hot paths.** Errors are `Result<T>` values. Exceptions are
  confined to the test harness and are caught at the HTTP boundary.

## Coding guidelines

The code follows a deliberate subset of **MISRA C++:2008** and **AUTOSAR C++14**:

* fixed-width integer types from `<cstdint>` everywhere on the wire and in the
  signal path;
* bounds-checked buffer reads (`cabin::Bytes`), no unchecked pointer arithmetic
  in the protocol code;
* no `new`/`delete` in the signal decode path; ownership is by value or
  `shared_ptr` at process boundaries;
* no RTTI; virtual dispatch is limited to service interfaces and the transport
  channel;
* explicit casts at the CAN and socket boundaries;
* `-Wall -Wextra -Wpedantic -Werror`, clang-format and clang-tidy in CI;
* `Result<T>` instead of exceptions for recoverable errors.

Deliberate deviations, documented rather than hidden:

* the STL (`std::vector`, `std::map`, `std::function`, threads) is used freely;
  MISRA C++:2008 predates these facilities and a full AUTOSAR rule set would
  require an approved container library;
* startup and publish paths allocate; only the per-frame decode/cache path is
  allocation-free;
* exceptions are enabled for the test framework and the HTTP handler boundary;
* `reinterpret_cast` appears at the POSIX socket boundary (`sockaddr`, CAN
  frames), which is unavoidable in portable code;
* floating-point CAN signals use IEEE-754 (HVAC setpoint), with finiteness and
  range checks; fixed-point would be preferable for a certified ASIL B signal.

## Known gaps before production

* No E2E counter/CRC per signal (CRC is per broker frame, not per CAN frame).
* No SecOC / authenticated CAN.
* No seat obstacle detection or pinch protection; ASIL B requires it.
* No redundant signal path or lock-step monitoring.
* Logging is a diagnostic aid, not a safety mechanism; it is best-effort and
  bounded by the queue policy.
