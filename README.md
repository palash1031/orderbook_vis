# Order Book Heatmap

A C++20 Coinbase Level 2 recorder, order-book reconstructor, and browser-based
live/replay liquidity heatmap viewer.

## Build

The project requires CMake 3.20 or newer, Boost with Boost.JSON, and OpenSSL.

```sh
cmake -S . -B build
cmake --build build --parallel
```

## Record Level 2 data

The recorder subscribes to a Coinbase Level 2 market and writes one WebSocket
message per line. `BTC-USD` remains the default:

```sh
./build/orderbook
./build/orderbook --product ETH-USD
./build/orderbook --product SOL-USD --output sol_capture.jsonl
```

Product IDs are trimmed, normalized to uppercase, and checked for `BASE-QUOTE`
syntax before connecting. Coinbase remains authoritative about whether a
syntactically valid product is actually listed. Without `--output`, the capture
name follows the product (`ETH-USD` becomes `eth_usd.jsonl`).

Stop the recorder with `Ctrl-C`.

## View a live heatmap

Start the loopback-only viewer with a Coinbase product and open
[http://127.0.0.1:8080](http://127.0.0.1:8080):

```sh
./build/heatmap_viewer --live --product SOL-USD
./build/heatmap_viewer --live --product DOGE-USD --price-bin 0.0001
```

`BTC-USD` and automatic product-aware price bins remain the defaults. The C++
process connects to Coinbase, validates message sequence, reconstructs the Level
2 book, samples the rolling heatmap, and fans columns out to browser clients over
`/ws/heatmap`. The browser never connects to Coinbase directly. A late browser
receives the retained rolling window before following new columns.

The dashboard marks a detected sequence gap and waits for a fresh snapshot
before resuming. Automatic network reconnect and resubscription are not yet
implemented, so restart the viewer after a Coinbase connection failure.

## Generate and view a replay heatmap

First replay a capture and export its rolling heatmap history:

```sh
./build/replay btc_usd.jsonl --heatmap-output heatmap.json
./build/replay doge_usd.jsonl --heatmap-output heatmap.json --price-bin 0.0001
```

The default `--price-bin auto` mode targets a total price span of approximately
0.25% across 201 rows, rounds to a readable 1/2/2.5/5 increment, and freezes
that resolution when the first two-sided book arrives. Automatic sizing is
price-aware but does not query Coinbase tick-size metadata; use a positive,
finite numeric `--price-bin` override when a market needs a specific grid.

Then start the viewer:

```sh
./build/heatmap_viewer --heatmap heatmap.json
```

Open [http://127.0.0.1:8080](http://127.0.0.1:8080). The viewer streams one
heatmap column at a time over a local WebSocket, paced by the recording's
exchange timestamps. Each browser session has independent playback state. The
viewer provides:

- play/pause, restart, and 1x/5x/20x replay controls;
- progress and replay-time readouts;
- bid, ask, or combined liquidity display;
- logarithmic intensity adjustment;
- a configurable visible price window;
- an optional midpoint trace; and
- cursor readings for time, price, and resting size.

Use `--port` to select another port or `--web-root` to serve a different copy
of the dashboard assets.

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Benchmarks

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=ON
cmake --build build-release --parallel
./build-release/orderbook_benchmarks
```
