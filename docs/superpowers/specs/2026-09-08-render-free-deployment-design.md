# Render Free Deployment Hardening

## Goal

Make the existing `heatmap_viewer` safe and operable as a public, read-only
Render Free Web Service while preserving its local defaults and all market-data
semantics.

## Scope

This slice adds:

- a configurable numeric bind address with `127.0.0.1` as the local default;
- `PORT` environment fallback with explicit `--port` precedence;
- a `--public-demo` policy that prevents browser clients from switching the
  process-wide live product;
- bounded browser WebSocket reconnection for network loss, cold starts, and
  platform restarts;
- a multi-stage production Docker image, `.dockerignore`, and a free Render
  Blueprint;
- deployment and local-container documentation; and
- a small recruiter-facing product identity and source link in the existing
  header.

It does not deploy the recorder, add credentials, add private exchange APIs,
change exchange protocols, change reconstruction or heatmap math, or advance
the multi-venue scanner branch.

## Configuration boundary

`ViewerOptions` remains the single parsed configuration value. It gains a bind
address and public-demo flag. Port selection is deterministic:

1. a valid explicit `--port` wins, even if `PORT` is malformed;
2. otherwise a present `PORT` is parsed with the same `1..65535` validation;
3. otherwise the existing port `8080` remains.

The bind address defaults to `127.0.0.1`. `--bind` accepts a numeric IPv4 or
IPv6 address and rejects hostnames or malformed values before assets or market
connections are opened. Render starts the viewer with `--bind 0.0.0.0`; the
recorder executable is unaffected.

The production `main` reads `PORT` once and supplies it to the pure parser as an
optional string. Tests inject that optional value directly and never mutate the
test process environment.

## Read-only control boundary

The existing live market selector changes one `LiveMarketService` shared by all
browser clients. The service therefore owns an explicit client-control access
mode. Interactive mode preserves current behavior. Read-only mode rejects
`apply_control()` before it can call `switch_product()`, leaving the selected
product, source session, and subscriber backlog unchanged.

`--public-demo` selects read-only mode only for live client controls. Replay
playback remains per-browser and therefore remains usable, and all local chart
controls continue to operate entirely in the browser. The server continues to
stream public market data normally and reports rejected live controls as its
existing WebSocket error message type.

## Browser recovery

The browser owns at most one connecting or open WebSocket plus at most one
scheduled retry. Unexpected closure schedules exponential delays beginning at
500 milliseconds and capped at 10 seconds. A successful `open` event clears the
retry and resets the attempt counter. A protocol-invalid stream is closed
deliberately and is not retried indefinitely.

During a retry the existing connection indicator and empty-chart message show
`Reconnecting…`; retained columns remain visible. Reconnection uses the current
page scheme and host, producing `wss:` automatically behind Render TLS.

## HTTP and container boundary

The existing `/health` route remains a cheap unauthenticated HTTP `200`. Render
routes HTTP and WebSocket upgrades to the one configured port.

The Docker builder installs only compilation dependencies and produces a
Release viewer with tests disabled. The runtime installs certificates and the
dynamic Boost.JSON/OpenSSL libraries, copies only `heatmap_viewer` and `web/`,
and runs as a non-root user from `/app`. Its default command starts the existing
single-market Coinbase `UNI-USD` live demo in public-demo mode and relies on
`PORT`; users may override that command for local testing.

`render.yaml` defines one Docker web service with `plan: free` and
`healthCheckPath: /health`. A free service can cold-start after inactivity, so
browser reconnect state is part of the required experience rather than a
market-data recovery change.

## Verification

Tests cover bind parsing, default bind and port, environment fallback, explicit
CLI precedence (including over a malformed environment value), malformed
environment ports, public-demo parsing, and rejection of shared live controls.
The complete deterministic CTest suite must remain green. Additional checks
validate JavaScript syntax, build the production Docker image when Docker is
available, run its `/health` endpoint locally, and confirm a same-origin
WebSocket reaches live state without enabling product switching.

## Platform references

- Render web-service binding and `PORT`: https://render.com/docs/web-services
- Render WebSocket behavior: https://render.com/docs/websocket
- Render Free Web Service limits: https://render.com/docs/free
- Render Blueprint schema: https://render.com/docs/blueprint-spec
