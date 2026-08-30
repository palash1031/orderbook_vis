# Order Book Heatmap

A C++20 Coinbase Level 2 recorder, order-book reconstructor, and browser-based
liquidity heatmap replay viewer.

## Build

The project requires CMake 3.20 or newer, Boost with Boost.JSON, and OpenSSL.

```sh
cmake -S . -B build
cmake --build build --parallel
```

## Record Level 2 data

The recorder subscribes to the Coinbase `BTC-USD` Level 2 channel and writes
one WebSocket message per line to `btc_usd.jsonl`.

```sh
./build/orderbook
```

Stop the recorder with `Ctrl-C`.

## Generate and view a replay heatmap

First replay a capture and export its rolling heatmap history:

```sh
./build/replay btc_usd.jsonl --heatmap-output heatmap.json
```

Then start the loopback-only viewer:

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
