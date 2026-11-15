# Transport mapping: HTTP, vSomeIP and gRPC

The transport is intentionally swappable. Everything above `cabin::gen::IRpcChannel`
speaks the same serialized structs; only the channel implementation changes.
`ServiceClient` is the in-repo Unix-socket channel. The gateway is a transcoder,
not a second API.

## REST → IDL

| HTTP | IDL | Request | Response |
| --- | --- | --- | --- |
| `GET /healthz` | none (gateway-local liveness) | – | `{"status":"ok","idl_version":"1.2"}` |
| `GET /v1/signals` | `SignalService.GetSignals` (0x0105/0x0001) | `Empty` | `SignalSnapshot` as JSON |
| `GET /v1/profiles/{id}` | `PersonalizationService.GetProfile` (0x0104/0x0001) | `GetProfileRequest` | `GetProfileResponse.profile` or 404 |
| `PUT /v1/profiles/{id}` | `PersonalizationService.PutProfile` (0x0104/0x0002) | `PutProfileRequest` | `{"accepted":true,"schema_version":1}` |
| `POST /v1/profiles/{id}/apply` | `PersonalizationService.ApplyProfile` (0x0104/0x0003) | `ApplyProfileRequest` | `{"applied":true,"latency_us":N}` |

The path's `{driver_id}` always overrides any `driver_id` in the JSON body. The
body is validated field-by-field against `generated/cabin_schema.json`; an
unknown field is ignored, a missing or wrong-typed field is a 400.

Every response carries `X-Cabin-Trace-Id`. If the request supplies one, the
gateway uses it verbatim; otherwise it mints a 64-bit id. The id is installed as
the thread-local trace id before the IDL call, so the frame carries it to the
service and every JSON log line on the path shares it.

## The same IDL on vSomeIP

vSomeIP is a service-oriented transport with service/instance/method/event IDs.
The mapping is direct:

| Cabin concept | vSomeIP |
| --- | --- |
| `service_id` (`0x0101`) | service ID (`0x0101`), instance `0x0001` |
| method id (`0x0001`) | method ID, `REQUEST`/`RESPONSE` message types |
| event id (`0x0001`) | event ID, eventgroup `0x0001`, `NOTIFICATION` |
| payload | the same `cabin_serdes` bytes |
| `idl_major` | interface version (major); minor is carried in payload |
| `seq` | SOME/IP client session / sequence counter |
| `trace_id` | not in the SOME/IP header; map into the 128-bit trace id or a header extension |
| `SERVICE_AVAILABLE/UNAVAILABLE` | SOME/IP-SD `OfferService` / `StopOfferService` |
| `FIND_SERVICE` | SOME/IP-SD `FindService` |
| `SUBSCRIBE` | SOME/IP-SD `SubscribeEventgroup` |
| error payload | SOME/IP return code + diagnostic string |

`cabin::MsgType` is deliberately shaped like the SOME/IP message types
(`REQUEST=1`, `RESPONSE=2`, `ERROR=3`, `NOTIFY`), so a future
`VSomeIpChannel : IRpcChannel` only replaces framing, not the generated API.
`ServiceClient`'s interface (`call(service, method, bytes, timeout)`) maps onto
vSomeIP's proxy `Request` call; `subscribe` maps onto eventgroup subscription.

## The same IDL on gRPC

The generated service names and method names transfer directly:

```proto
service SeatService {
  rpc SetPosition(SeatSetPositionRequest) returns (SeatSetPositionResponse);
  rpc GetState(Empty) returns (SeatState);
}
service PersonalizationService {
  rpc GetProfile(GetProfileRequest) returns (GetProfileResponse);
  rpc PutProfile(PutProfileRequest) returns (PutProfileResponse);
  rpc ApplyProfile(ApplyProfileRequest) returns (ApplyProfileResponse);
}
```

A gRPC channel would serialize the same `Bytes` payload into a `bytes` field or
use generated protobuf messages; the Rust/C++ service base remains unchanged.
Mapping details:

* HTTP status: `NOT_FOUND`→`NOT_FOUND`, `OUT_OF_RANGE`/`INVALID_ARGUMENT`→
  `INVALID_ARGUMENT`, `TIMEOUT`→`DEADLINE_EXCEEDED`, `UNAVAILABLE`→
  `UNAVAILABLE`, `VERSION_MISMATCH`→`FAILED_PRECONDITION`.
* Trace: `x-cabin-trace-id` metadata, installed with `set_current_trace_id`.
* Events: server-streaming RPC per service event (or a single
  `WatchSignals` stream carrying `SignalSnapshot`).

## Why this matters

The REST surface is a *view*, not a protocol fork. Adding vSomeIP or gRPC does
not change the IDL, the generated types, the range checks or the trace model.
That is the property this middleware is built to demonstrate.
