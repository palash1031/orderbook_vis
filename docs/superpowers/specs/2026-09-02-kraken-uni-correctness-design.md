# Kraken UNI/USD Correctness Vertical Slice

## Goal

Add a second live venue without introducing multi-product orchestration yet. The
viewer must run one active Kraken spot book through:

```sh
./build/heatmap_viewer --live --venue kraken --product UNI-USD
```

The Kraken adapter owns Kraken naming, parsing, book maintenance, checksum, and
recovery. The rest of the application sees canonical product identity and only
trusted book events.

Kraken's public AssetPairs API reported `UNIUSD` online with WebSocket name
`UNI/USD` on 2026-09-02. Runtime instrument discovery remains authoritative;
this observation is test evidence, not a hardcoded listing guarantee.

## Scope

This chunk includes:

- minimal canonical `Product`, `Venue`, and `MarketKey` types;
- Kraken WebSocket v2 instrument discovery for one configured product;
- a single Kraken book subscription, default depth 500;
- exact wire-decimal preservation for Kraken checksum state;
- checksum-validated snapshot and atomic delta reconstruction;
- invalidation, reconnect, and fresh-snapshot recovery;
- viewer venue selection and an end-to-end Kraken live path;
- deterministic unit/integration fixtures and an opt-in live smoke test; and
- documentation of the new command and integrity behavior.

This chunk excludes the twelve-product `MarketUniverse`, multiplexed sessions,
multi-dimensional book storage, consolidated quotes, fragmentation, execution
simulation, and scanner UI. Those remain later chunks.

## Architecture

### Canonical identity

Core owns validated, comparable canonical identities:

- `Product { base, quote }`, formatted as `BASE-QUOTE`;
- `Venue`, initially `Coinbase` and `Kraken`; and
- `MarketKey { product, venue }`.

No core type contains Kraken aliases or slash-formatting rules. Existing string
product IDs may remain at compatibility boundaries, but Kraken parsing resolves
native symbols to `Product` before publishing data.

### Kraken catalog and transport

One TLS WebSocket connection opens Kraken's public v2 endpoint. It subscribes to
the `instrument` channel, parses the snapshot, resolves the configured canonical
product from the pair's `base` and `quote` metadata, and records the returned
native `symbol`. Only then does it subscribe to the `book` channel for that
symbol at depth 500.

Missing, disabled, or non-online pairs become an unsupported-market outcome.
They do not add a product alias to common code and are not retried forever as a
transport failure. Subscription acknowledgements, heartbeats, and status
messages do not alter trusted book state.

### Exact Kraken book

The Kraken parser preserves the original JSON numeric token for every price and
quantity. A validated exact decimal representation supplies numeric ordering
without collapsing distinct price identities through binary floating point.
Prices and quantities must be finite, non-negative canonical decimal tokens;
zero is permitted only as a deletion quantity.

Snapshots replace both book sides. Updates are processed in their native array
order, including repeated changes to the same price. After the complete batch is
applied, each side is truncated to the subscribed depth. CRC32 is then computed
over the top ten asks in ascending price order followed by the top ten bids in
descending price order, removing decimal points and leading zeros exactly as
specified by Kraken.

### Trust boundary and heatmap flow

`KrakenBookAdapter` starts unsynchronized. A checksum-valid snapshot establishes
trust and emits a canonical authoritative snapshot. A checksum-valid update batch
emits one atomic trusted delta. Conversion to `double` occurs only after checksum
validation for the existing visualization boundary.

The common heatmap engine accepts trusted snapshots, atomic deltas, and
invalidations. It does not inspect native Kraken symbols or apply Kraken checksum
rules. Coinbase keeps its existing feed-level sequence tracker, including
non-book messages; no Kraken behavior is routed through that tracker.

## Failure and recovery

Malformed book data, an unexpected native symbol, invalid decimals, an invalid
depth, or a checksum mismatch invalidates the Kraken book immediately. No part
of the failing batch is published. Further updates are ignored until a fresh,
checksum-valid snapshot arrives.

Transport closure and integrity failure both mark the heatmap discontinuous,
publish waiting/reconnecting state, and open a new connection using the existing
exponential backoff policy. Backoff resets only after a trusted live snapshot or
update. A new connection repeats instrument discovery and subscription; retained
heatmap history is not retroactively rewritten.

## Testing and proof

All new behavior is covered test-first at the smallest useful boundary:

1. canonical identity normalization, validation, comparison, and venue parsing;
2. instrument catalog parsing and canonical/native resolution for `UNI-USD`;
3. supported depths and subscription JSON;
4. Kraken's published checksum vector with literal expected CRC32;
5. captured or deterministic UNI snapshot/update fixtures;
6. exact price identity, update ordering, deletion, and depth truncation;
7. checksum failure, invalidation, ignored deltas, and fresh-snapshot recovery;
8. scripted reconnect ordering and unsupported-subscription handling;
9. viewer `--venue kraken --product UNI-USD` configuration; and
10. full existing Coinbase/replay regression tests.

An opt-in network smoke test may connect to Kraken and require a trusted UNI book
snapshot. It is not part of the deterministic default suite.

## Protocol references

- Kraken Spot WebSocket v2 Book (Level 2):
  https://docs.kraken.com/exchange/api-reference/spot-websocket-v2/book
- Kraken Spot WebSocket v2 Instruments:
  https://docs.kraken.com/exchange/api-reference/spot-websocket-v2/instrument
- Kraken WebSocket v2 book checksum guide:
  https://docs.kraken.com/exchange/guides/websockets/book-checksum-v2
- Kraken public AssetPairs observation for UNI/USD:
  https://api.kraken.com/0/public/AssetPairs?pair=UNIUSD
