# API versioning policy

The cabin API is versioned in two places:

* the **wire version** stamped on every frame (`idl_major`, `idl_minor`), and
* the **schema version** carried in persisted profiles (`schema_version`).

Both follow the same rule: a major change is incompatible and rejected; a minor
change is additive and must be ignorable by older peers.

## Wire version negotiation

`include/cabin/frame.hpp` rejects a frame whose `idl_major` differs from the
compiled-in value with `ErrorCode::kVersionMismatch`. A different `idl_minor` is
accepted in both directions. `Frame.VersionNegotiation` covers major 2, minor 99
and minor 0.

## What is frozen in v1

The following are **frozen** for the lifetime of major version 1:

* service IDs (`SeatService` 0x0101, `HvacService` 0x0102, `LightingService`
  0x0103, `PersonalizationService` 0x0104, `SignalService` 0x0105);
* method and event IDs per service;
* field order and field types of every struct;
* enum numeric values (new values may be appended only).

`GetProfile`, `PutProfile` and `ApplyProfile` keep their request/response shapes.

## Additive changes in v1.2

`Profile.preferred_language` is the worked example. It is declared in
`idl/cabin.yaml` as:

```json
{"name": "preferred_language", "type": "string", "max_len": 16,
 "since_minor": 2, "default": "en"}
```

Rules enforced by `tools/idlc.py`:

1. A `since_minor` field must be the **last** field of its struct.
2. It must carry a default.
3. On decode, if the remaining nested buffer is empty, the default is used.
4. On decode, trailing bytes beyond the last known field are ignored, so a
   v1.2 producer can talk to a v1.0 consumer.
5. Nested structs are length-prefixed, which keeps an older decoder from
   consuming a newer sibling field as part of the nested value.

Both directions are tested:

* `Serdes.AdditiveV12FieldDefaultsWhenAbsentAndTrailingIsIgnored` appends a
  hypothetical v1.3 field and removes the v1.2 field.
* `Personalization.LoadsV1PayloadWithAdditiveV12FieldAndUnknownFields` loads a
  hand-written v1.0 profile JSON file with an unknown future field and checks
  that `preferred_language` falls back to `"en"`.

## Deprecation rule

A field or method is deprecated by:

1. documenting it as deprecated in `docs/api_v1.md` and the IDL comment,
2. keeping encode/decode behaviour and IDs unchanged for at least one **minor**
   release (minimum 6 months),
3. removing it only in the next **major** release.

Clients must never reject unknown fields, unknown enum values, or a higher minor
version. Servers must never reuse an ID or change a field type in a minor
release.

## Persisted profiles

`profiles.json` carries `schema_version`. The store accepts version 1 and
validates every field against the generated ranges. A file that cannot be
parsed is reported (`PROFILE_LOAD_FAILED`) and left on disk untouched; it is not
silently replaced. A future schema version will be handled by a loader that maps
known fields and defaults the rest, following the same additive rule.
