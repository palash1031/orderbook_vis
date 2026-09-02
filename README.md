# Order Book Heatmap

A C++20 Coinbase/Kraken Level 2 market-data engine, order-book reconstructor,
and browser-based live/replay liquidity heatmap viewer.

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

To run the checksum-validated Kraken v2 vertical slice on canonical
`UNI-USD`:

```sh
./build/heatmap_viewer --live --venue kraken --product UNI-USD
```

The Kraken connection first requests the venue's live instrument catalog and
resolves canonical `UNI-USD` to the native symbol currently advertised by
Kraken (normally `UNI/USD`). The listing is not hardcoded: a missing or
non-online pair is reported as unsupported and is not retried as though it were
a temporary network failure.

After discovery, the same WebSocket session subscribes to one Level 2 book at
depth 500. Kraken price and quantity tokens remain exact through reconstruction;
each complete snapshot or update batch is applied in venue order, truncated to
the subscribed depth, and checked with Kraken's top-ten CRC32 procedure before
the common heatmap sees it. A checksum or protocol failure invalidates the book,
opens a fresh session through exponential backoff, repeats discovery, and
ignores updates until a new checksum-valid snapshot establishes trust.

The heatmap visualizes observed resting depth. It is not a promise that the
displayed quantity can be filled at those prices; latency, cancellations, queue
position, fees, and exchange matching behavior still matter.

`BTC-USD` and automatic product-aware price bins remain the defaults. The C++
process connects to Coinbase, validates message sequence, reconstructs the Level
2 book, samples the rolling heatmap, and fans columns out to browser clients over
`/ws/heatmap`. The browser never connects to Coinbase directly. A late browser
receives the retained rolling window before following new columns.

In live mode, use the market selector in the header to switch between BTC-USD,
ETH-USD, SOL-USD, and DOGE-USD without restarting the viewer. A switch is global
to the local viewer process and all connected browser tabs. It invalidates the
old source immediately, clears the previous product's columns, opens a new
Coinbase subscription, and waits for that product's fresh snapshot before
drawing. Automatic price resolution is selected again for the new product; a
manual `--price-bin` remains fixed across selections.

The viewer also subscribes to Coinbase heartbeats so quiet markets keep their
connection open. When the socket closes or a sequence gap is detected, it marks
the retained chart stale, reconnects with exponential backoff and jitter,
resubscribes, and refuses further updates until a fresh Level 2 snapshot rebuilds
the book. The frozen price grid and rolling history survive recovery, with a
visible discontinuity between the old and newly synchronized data. Subscription
errors such as an unknown product remain visible and are not retried endlessly.

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

The deterministic suite uses scripted Kraken frames and skips public-network
access. To opt into a real Kraken `UNI/USD` snapshot proof:

```sh
ORDERBOOK_RUN_KRAKEN_LIVE_TESTS=1 \
  ctest --test-dir build --output-on-failure -R KrakenLiveSmokeTest
```

## Benchmarks

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=ON
cmake --build build-release --parallel
./build-release/orderbook_benchmarks
```
