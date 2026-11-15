# Deployment

CabinServiceMiddleware runs as a small set of user-space services around one
broker. The production topology is deliberately boring: a single Unix-domain
socket, per-service processes, and systemd responsible for restart and isolation.

## Process topology

```
                       /run/cabin/broker.sock
  cabin-gateway  ─────────────┐  (HTTP :8080)
  personalizationd  ──────────┤
  cabin-services (seat/hvac/lighting) ──┤
  signald (CAN replay / SocketCAN) ─────┤
                                   ┌────┴────┐
                                   │ cabind  │  service registry, routing,
                                   └─────────┘  pub/sub fan-out
```

* `cabind` owns the broker socket and no other resource.
* `signald` owns the CAN interface (or a capture file) and publishes typed
  signal events. It hosts `SignalService` for reads and actuation.
* `cabin-services` hosts `SeatService`, `HvacService` and `LightingService`.
* `personalizationd` hosts `PersonalizationService` and persists profiles.
* `cabin-gateway` accepts HTTP/1.1 on a loopback or vehicle-internal interface
  and transcodes to IDL calls. It never reaches into another process directly.

Every process discovers services by ID through `FIND_SERVICE`; no process has a
compiled-in endpoint. A provider calling `OFFER_SERVICE` receives a
`SERVICE_AVAILABLE` notification at every connected consumer. When a provider
disconnects, the broker broadcasts `SERVICE_UNAVAILABLE` and fails in-flight
requests routed to it with `UNAVAILABLE` rather than leaking a timeout.

## Runtime directories

| Variable | Default | Purpose |
| --- | --- | --- |
| `CABIN_RUNTIME_DIR` | `/tmp/cabin` | Socket and transient runtime state |
| `CABIN_STATE_DIR` | `$CABIN_RUNTIME_DIR/state` | Personalization profiles |
| `CABIN_LOG_LEVEL` | `info` | `trace|debug|info|warn|error|off` |
| `CABIN_LOG_FILE` | stderr | Append JSON-lines logs to a file |

On a real vehicle `/run/cabin` and `/var/lib/cabin` are tmpfs and persistent
storage respectively, provided by systemd's `RuntimeDirectory=` and
`StateDirectory=`. The unit files in `systemd/` already do this.

## Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo install -m 0755 build/cabind build/signald build/cabin-services \
  build/personalizationd build/cabin-gateway /usr/local/bin/
sudo useradd --system --home /var/lib/cabin --shell /usr/sbin/nologin cabin
sudo install -d -o cabin -g cabin /var/lib/cabin
sudo install -m 0644 systemd/cabind.service systemd/cabin-gateway.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cabind
sudo systemctl enable --now cabin-gateway
```

The units are intentionally not socket-activated yet. To socket-activate, add a
`.socket` unit with `ListenStream=/run/cabin/broker.sock`, pass the inherited
descriptor to `cabind` (a future `--socket-fd` option), and drop
`RuntimeDirectory=` from the service. The broker is written so that this only
changes process startup, not the protocol.

## Hardening

Both units set `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`,
`RestrictNamespaces`, a `@system-service` syscall filter and
`RestrictAddressFamilies` limited to the families each daemon actually uses
(`AF_UNIX` for the broker; `AF_UNIX` plus `AF_INET/AF_INET6` for the HTTP
gateway). The gateway can only write inside `/var/lib/cabin`.

## Failure behaviour

* Broker dies: clients' reader threads observe EOF, pending RPCs fail with
  `UNAVAILABLE`, and `systemd` restarts `cabind`. `ServiceClient::reconnect()`
  re-offers services and re-subscribes to events. This path is covered by
  `Broker.BrokerRestartAndClientReconnect`.
* Consumer dies: broker removes its subscriptions and provider registrations.
* A slow subscriber: the broker's per-client queue applies drop-oldest to
  events only, logs `queue_overflow`, and the counter is surfaced in the
  benchmark output. Request/response traffic is never intentionally dropped.
* Out-of-range CAN data: `signald` clamps the value, logs `signal_out_of_range`
  and continues; the raw frame never reaches a service unchecked.
