# Kraken UNI/USD Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a one-product Kraken WebSocket v2 path that discovers `UNI/USD`, reconstructs and CRC32-validates its Level 2 book, and supplies only trusted canonical `UNI-USD` state to the existing live heatmap.

**Architecture:** Core receives minimal `Product`, `Venue`, `MarketKey`, and trusted-book event types. Kraken-specific catalog, exact wire-decimal state, checksum, and synchronization live behind `KrakenBookAdapter`; a single Kraken stream performs instrument discovery and one depth-500 book subscription. The existing live runner gains a trusted-event path while Coinbase keeps its current feed-level sequence tracker unchanged.

**Tech Stack:** C++20, Boost.JSON, Boost.CRC, Boost.Asio/Beast TLS WebSockets, OpenSSL, CMake 3.20+, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-09-02-kraken-uni-correctness-design.md`

## Global Constraints

- The production vertical slice has one active product per Kraken connection; do not add `MarketUniverse`, multiplexing, consolidation, routing, or scanner UI.
- Use canonical `BASE-QUOTE` identity outside venue adapters; Kraken slash symbols and aliases never enter common routing or heatmap code.
- Kraken instrument metadata is runtime truth. Do not hardcode `UNI/USD` or a product whitelist in `OrderBook`, the parser, or the live runner.
- Subscribe to Kraken book depth 500 by default and accept only `10`, `25`, `100`, `500`, or `1000`.
- Process an entire Kraken update batch in native order, truncate afterward, then validate CRC32 over the top ten asks followed by the top ten bids.
- Preserve exact numeric wire tokens through checksum validation; convert to `double` only for the existing visualization boundary.
- On checksum/protocol failure, publish invalidation and accept no update until a fresh valid snapshot.
- Existing Coinbase sequencing, recorder, replay, live viewer, switching, and recovery behavior must remain green.
- Every production behavior begins with a witnessed failing test.

---

### Task 1: Canonical market identity and viewer configuration

**Files:**
- Create: `include/market.hpp`
- Create: `src/market.cpp`
- Create: `include/viewer_config.hpp`
- Create: `src/viewer_config.cpp`
- Create: `tests/market_tests.cpp`
- Create: `tests/viewer_config_tests.cpp`
- Modify: `src/recorder_config.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `Product::Product(base, quote)`, `Product::parse(text)`, `Product::to_string()`, `Venue`, `venue_name(Venue)`, `parse_venue(text)`, `MarketKey`, and `parse_viewer_options(...)`.
- Preserves: `normalize_product_id(text)` remains a compatibility function and delegates canonical validation to `Product`.

- [ ] **Step 1: Add failing identity and CLI tests**

Add tests equivalent to:

```cpp
TEST(MarketTest, ProductNormalizesCanonicalIdentity)
{
    const Product product{" uni ", "usd"};
    EXPECT_EQ(product.base(), "UNI");
    EXPECT_EQ(product.quote(), "USD");
    EXPECT_EQ(product.to_string(), "UNI-USD");
    EXPECT_EQ(Product::parse(" uni-usd "), product);
}

TEST(ViewerConfigTest, AcceptsKrakenAndUni)
{
    constexpr std::array<std::string_view, 5> args{
        "--live", "--venue", "kraken", "--product", " uni-usd "
    };
    const ViewerOptions options = parse_viewer_options(args);
    EXPECT_EQ(options.venue, Venue::Kraken);
    EXPECT_EQ(options.product_id, "UNI-USD");
}
```

Also cover malformed components/pairs, hashing and ordering, venue parsing,
duplicate `--venue`, invalid venue, replay-mode venue rejection, and Coinbase as
the default live venue.

- [ ] **Step 2: Run the focused build and witness failure**

Run:

```sh
cmake -S . -B build
cmake --build build --target orderbook_tests --parallel
```

Expected: compilation fails because `market.hpp`, `viewer_config.hpp`, and their
symbols do not exist.

- [ ] **Step 3: Implement the canonical types**

Create the following public shape:

```cpp
class Product
{
public:
    Product(std::string_view base, std::string_view quote);
    static Product parse(std::string_view text);
    const std::string& base() const noexcept;
    const std::string& quote() const noexcept;
    std::string to_string() const;
    auto operator<=>(const Product&) const = default;
private:
    std::string base_;
    std::string quote_;
};

enum class Venue { Coinbase, Kraken };

struct MarketKey
{
    Venue venue;
    Product product;
    auto operator<=>(const MarketKey&) const = default;
};
```

Trim ASCII whitespace, uppercase components, allow only ASCII letters/digits,
and reject empty or over-32-character components. Implement stable hashes for
`Product` and `MarketKey`. Move viewer argument parsing into
`parse_viewer_options`; keep `BTC-USD` and Coinbase as defaults, but accept
`--live --venue kraken --product UNI-USD`.

- [ ] **Step 4: Run focused tests and full existing configuration tests**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'MarketTest|ViewerConfigTest|RecorderConfigTest|ReplayConfigTest'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit the canonical identity slice**

```sh
git add CMakeLists.txt include/market.hpp include/viewer_config.hpp src/market.cpp src/viewer_config.cpp src/recorder_config.cpp tests/market_tests.cpp tests/viewer_config_tests.cpp
git commit -m "Add canonical market identity"
```

---

### Task 2: Trusted-book event boundary for the live heatmap

**Files:**
- Create: `include/venue_adapter.hpp`
- Create: `src/live_message_source.cpp`
- Modify: `include/live_message_source.hpp`
- Modify: `include/live_heatmap_engine.hpp`
- Modify: `src/live_heatmap_engine.cpp`
- Modify: `include/live_source.hpp`
- Modify: `src/live_source.cpp`
- Modify: `include/live_stream.hpp`
- Modify: `tests/live_heatmap_engine_tests.cpp`
- Modify: `tests/live_source_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Product`, `Venue`, and `MarketKey` from Task 1.
- Produces: `VenueSessionStatus`, `VenueMarketStatus`, `TrustedBookEventType`, `TrustedBookEvent`, `VenueAdapter`, and `TrustedLiveMessageSource::read_event()`.
- Adds: `LiveHeatmapEngine::process(const TrustedBookEvent&)` without changing `process(std::string_view)` Coinbase behavior.

- [ ] **Step 1: Add failing trusted-event heatmap tests**

Construct canonical Kraken events with a small `OrderBook` and verify:

```cpp
TrustedBookEvent snapshot{
    TrustedBookEventType::Snapshot,
    {Venue::Kraken, Product("UNI", "USD")},
    timestamp,
    book
};
EXPECT_EQ(engine.process(snapshot).status_change,
          LiveHeatmapStatus::Live);
```

Add tests proving an update is ignored while waiting for a snapshot, an
invalidation returns to `WaitingForSnapshot`, a subsequent snapshot recovers,
and a mismatched canonical product throws.

- [ ] **Step 2: Run focused tests and witness failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
```

Expected: compilation fails because trusted-event types and overloads are absent.

- [ ] **Step 3: Implement the trust boundary**

Define:

```cpp
enum class VenueSessionStatus
{
    Connecting, Connected, Disconnected, Reconnecting
};

enum class VenueMarketStatus
{
    Unsupported, Connecting, WaitingForSnapshot, Live, Stale, Reconnecting
};

enum class TrustedBookEventType { Snapshot, Update, Invalidated };

struct TrustedBookEvent
{
    TrustedBookEventType type;
    MarketKey market;
    MarketTimestamp timestamp;
    std::optional<OrderBook> book;
};
```

`LiveHeatmapEngine::process(event)` must require the configured canonical
product, call `begin_recovery()` for invalidation, reject missing book state,
ignore updates before a snapshot, sample complete trusted book state, and retain
the current heatmap grid across recovery.

Add `TrustedLiveMessageSource`, detected by the runner as a trusted-event source.
After publishing an invalidation event, force reconnect. Parameterize diagnostic
text with `source_name` while keeping Coinbase defaults.

- [ ] **Step 4: Run trusted-event and Coinbase regression tests**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'LiveHeatmapEngineTest|LiveSourceRunnerTest|LiveMarketServiceTest|SequenceTrackerTest'
```

Expected: all selected tests pass, including existing Coinbase sequence-gap and
non-book-channel cases.

- [ ] **Step 5: Commit the trusted event boundary**

```sh
git add CMakeLists.txt include/venue_adapter.hpp include/live_message_source.hpp include/live_heatmap_engine.hpp include/live_source.hpp include/live_stream.hpp src/live_message_source.cpp src/live_heatmap_engine.cpp src/live_source.cpp tests/live_heatmap_engine_tests.cpp tests/live_source_tests.cpp
git commit -m "Add trusted live book events"
```

---

### Task 3: Kraken catalog, lossless decimals, and checksum-valid book

**Files:**
- Create: `include/kraken_adapter.hpp`
- Create: `src/kraken_adapter.cpp`
- Create: `tests/kraken_adapter_tests.cpp`
- Create: `tests/fixtures/kraken_v2_checksum_snapshot.json`
- Create: `tests/fixtures/kraken_uni_book.jsonl`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: canonical identity and trusted-book events from Tasks 1-2.
- Produces: `KrakenInstrumentCatalog`, `KrakenBookAdapter`, `is_supported_kraken_book_depth`, `make_kraken_instrument_subscription`, and `make_kraken_book_subscription`.

- [ ] **Step 1: Add failing catalog and subscription tests**

Use an instrument fixture containing:

```json
{"symbol":"UNI/USD","base":"UNI","quote":"USD","status":"online"}
```

Verify bidirectional resolution between `Product("UNI", "USD")` and
`UNI/USD`, exclusion of non-online pairs, rejection of missing/ambiguous fields,
the exact supported-depth set, and a depth-500 book request containing
`"symbol":["UNI/USD"]`.

- [ ] **Step 2: Run the Kraken adapter test target and witness failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
```

Expected: compilation fails because `kraken_adapter.hpp` is absent.

- [ ] **Step 3: Implement catalog parsing and request builders**

Parse only `channel=instrument`, `type=snapshot`; construct canonical products
from `base` and `quote` metadata rather than splitting native symbols. Retain
only `status=online` mappings and reject duplicate canonical or native keys.
Generate Kraken v2 JSON requests with snapshot enabled and validate depth before
serialization.

- [ ] **Step 4: Add failing checksum and reconstruction tests**

Cover:

- Kraken's published snapshot with expected CRC32 `3310070434`;
- a deterministic `UNI/USD` snapshot and checksum-valid update;
- unquoted values such as `101.00` and `0.0100` retaining checksum zeros;
- two exact prices that collide when converted to `double`;
- zero-quantity deletion and repeated same-price updates in native order;
- complete-batch application before depth truncation;
- mismatched symbols, invalid/non-finite/negative decimals, incomplete levels,
  empty snapshots, and invalid checksum values; and
- invalidation followed by ignored updates and a fresh valid snapshot.

Expected test shape:

```cpp
KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
const auto snapshot = adapter.process(uni_snapshot);
ASSERT_EQ(snapshot->type, TrustedBookEventType::Snapshot);
EXPECT_EQ(snapshot->market.product, Product("UNI", "USD"));

const auto invalid = adapter.process(corrupt_update);
ASSERT_EQ(invalid->type, TrustedBookEventType::Invalidated);
EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale);
EXPECT_FALSE(adapter.process(valid_update).has_value());
```

- [ ] **Step 5: Run the focused tests and witness behavioral failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'KrakenCatalogTest|KrakenBookAdapterTest'
```

Expected: catalog tests may pass after Step 3, while checksum/reconstruction tests
fail because book state and CRC32 are not implemented.

- [ ] **Step 6: Implement lossless book reconstruction**

Use a narrow lexical extractor for the `asks` and `bids` arrays so price and
quantity JSON tokens remain exact whether quoted or unquoted. Validate decimal
grammar before `strtod`, create a trailing-zero-normalized key for exact numeric
identity, compare keys without binary floating point, and retain the latest wire
text for checksum input.

For each book message: copy prior state for updates or start empty for snapshots,
apply all asks and bids in array order, require both sides for a snapshot,
truncate, calculate CRC32 with `boost::crc_32_type`, compare to the unsigned
32-bit field, and publish a full `OrderBook` only on success. On any exception or
checksum mismatch, clear state and return `Invalidated`.

- [ ] **Step 7: Run all adapter tests**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'MarketTest|KrakenCatalogTest|KrakenBookAdapterTest'
```

Expected: all selected tests pass, including literal checksum and exact-price
identity cases.

- [ ] **Step 8: Commit the trusted Kraken book**

```sh
git add CMakeLists.txt include/kraken_adapter.hpp src/kraken_adapter.cpp tests/kraken_adapter_tests.cpp tests/fixtures/kraken_v2_checksum_snapshot.json tests/fixtures/kraken_uni_book.jsonl
git commit -m "Add checksum-valid Kraken book adapter"
```

---

### Task 4: One-session Kraken discovery, reconnect, and fresh-snapshot recovery

**Files:**
- Create: `include/kraken_level2_stream.hpp`
- Create: `src/kraken_level2_stream.cpp`
- Create: `tests/kraken_stream_tests.cpp`
- Modify: `include/venue_adapter.hpp`
- Modify: `src/live_source.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Kraken request/catalog/adapter interfaces from Task 3 and `TrustedLiveMessageSource` from Task 2.
- Produces: `KrakenWire` test seam, `KrakenLevel2Stream(product, depth)`, injected-wire constructor, and `UnsupportedMarketError`.

- [ ] **Step 1: Add failing scripted stream tests**

Create a fake wire that records writes and yields an instrument acknowledgement,
instrument snapshot with `UNI/USD`, book acknowledgement, and trusted book
snapshot. Verify exactly two subscription writes occur on one wire session and
the second request uses `UNI/USD` at depth 500.

Add tests for malformed discovery messages, subscription rejection, missing or
non-online `UNI-USD`, non-book messages being skipped, and a valid trusted
snapshot being returned.

- [ ] **Step 2: Run stream tests and witness failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
```

Expected: compilation fails because the Kraken stream and wire seam do not exist.

- [ ] **Step 3: Implement the Kraken v2 transport and handshake**

Open `wss://ws.kraken.com/v2` using the same peer verification, SNI, and Beast
timeout patterns as Coinbase. On a single connection:

1. write the instrument subscription;
2. ignore acknowledgement/status/heartbeat messages while waiting for an
   instrument snapshot;
3. resolve canonical `UNI-USD` through `KrakenInstrumentCatalog`;
4. throw `UnsupportedMarketError` if no online pair is present;
5. construct the adapter and write the depth-500 book subscription; and
6. return only adapter-produced trusted events from `read_event()`.

- [ ] **Step 4: Add failing reconnect and recovery-order tests**

Use two fake sessions. Session one emits snapshot then corrupt update. Session
two emits an update before its snapshot, then a valid fresh snapshot. Verify the
observed trusted events are exactly:

```text
session 0 Snapshot
session 0 Invalidated
session 1 Snapshot
```

Verify waiting/reconnecting state appears before the second snapshot and live
state only afterward. Add a runner test proving `UnsupportedMarketError` is
published as unsupported/disconnected and does not create a retry loop.

- [ ] **Step 5: Run recovery tests and witness failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'KrakenLevel2StreamTest'
```

Expected: the basic handshake may pass after Step 3; recovery ordering and
non-retryable unsupported behavior fail until runner handling is added.

- [ ] **Step 6: Implement reconnect integration and unsupported handling**

On a trusted invalidation, publish heatmap recovery before throwing into the
existing reconnect loop. Construct a fresh stream/adapter per retry, so updates
cannot restore trust before the new snapshot. Catch `UnsupportedMarketError`
outside the retry path, publish a terminal visible error/status for the selected
market, and return without sleeping or reconnecting.

- [ ] **Step 7: Run stream, runner, and Coinbase recovery suites**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R 'KrakenLevel2StreamTest|LiveSourceRunnerTest|LiveMarketServiceTest'
```

Expected: all selected tests pass and Kraken recovery ordering is deterministic.

- [ ] **Step 8: Commit transport and recovery**

```sh
git add CMakeLists.txt include/kraken_level2_stream.hpp include/venue_adapter.hpp src/kraken_level2_stream.cpp src/live_source.cpp tests/kraken_stream_tests.cpp
git commit -m "Stream trusted Kraken UNI books"
```

---

### Task 5: Viewer wiring, documentation, and opt-in live proof

**Files:**
- Modify: `src/heatmap_viewer.cpp`
- Modify: `README.md`
- Modify: `tests/viewer_config_tests.cpp`
- Modify: `tests/kraken_stream_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ViewerOptions::venue` and `KrakenLevel2Stream`.
- Produces: end-to-end `heatmap_viewer --live --venue kraken --product UNI-USD` behavior while preserving replay and Coinbase defaults.

- [ ] **Step 1: Add failing viewer-factory and live-smoke tests**

Strengthen the viewer configuration test to require canonical `UNI-USD` for the
documented command. Add an opt-in GoogleTest guarded by
`ORDERBOOK_RUN_KRAKEN_LIVE_TESTS=1`:

```cpp
if (std::getenv("ORDERBOOK_RUN_KRAKEN_LIVE_TESTS") == nullptr)
    GTEST_SKIP() << "set ORDERBOOK_RUN_KRAKEN_LIVE_TESTS=1";
KrakenLevel2Stream stream("UNI-USD", 10);
EXPECT_EQ(stream.read_event().type, TrustedBookEventType::Snapshot);
```

- [ ] **Step 2: Run focused tests and witness the missing wiring**

Run:

```sh
cmake --build build --target orderbook_tests heatmap_viewer --parallel
ctest --test-dir build --output-on-failure -R 'ViewerConfigTest|KrakenLiveSmokeTest'
```

Expected: deterministic configuration tests pass; the smoke test is skipped;
manual CLI inspection still shows the viewer factory always selects Coinbase.

- [ ] **Step 3: Wire venue selection into the viewer**

Move the existing in-file option parser to `viewer_config.cpp`. In `load_state`,
select one source factory from `options.venue`:

```cpp
if (venue == Venue::Kraken)
    return std::make_unique<KrakenLevel2Stream>(product_id);
return std::make_unique<CoinbaseLevel2Stream>(product_id);
```

Pass `venue_name(options.venue)` to diagnostics and include the venue in health
and live metadata JSON. Do not alter browser product-switch semantics in this
chunk; each switch still tears down the one active product session.

- [ ] **Step 4: Document the vertical slice**

Update `README.md` with the exact UNI command, runtime symbol discovery,
depth-500 retention, per-message CRC32 validation, reconnect/fresh-snapshot
behavior, and the warning that the viewer shows observed depth rather than fill
guarantees. Keep the Coinbase and replay instructions intact.

- [ ] **Step 5: Run deterministic verification**

Run:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Expected: build succeeds and the entire deterministic suite passes with only the
network smoke test skipped.

- [ ] **Step 6: Run the opt-in Kraken proof when network is available**

Run:

```sh
ORDERBOOK_RUN_KRAKEN_LIVE_TESTS=1 ctest --test-dir build --output-on-failure -R KrakenLiveSmokeTest
```

Expected: the public Kraken v2 connection resolves `UNI/USD` and returns a
checksum-valid snapshot. If the environment blocks network access, record the
sandbox/network error separately; do not weaken deterministic tests.

- [ ] **Step 7: Commit viewer integration and docs**

```sh
git add CMakeLists.txt README.md src/heatmap_viewer.cpp tests/viewer_config_tests.cpp tests/kraken_stream_tests.cpp
git commit -m "Expose Kraken UNI live heatmap"
```

---

### Task 6: Final regression and scope audit

**Files:**
- Verify only; modify earlier files only if verification exposes a defect.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: evidence that Chunk 1 is complete and later chunks were not pulled in.

- [ ] **Step 1: Inspect changed files and forbidden scope**

Run:

```sh
git status --short
git diff --stat 4d22024..HEAD
rg -n "MarketUniverse|MarketStore|best.execution|fragmentation|scanner" include src web tests
```

Expected: no production implementation of later-chunk universe/store/scanner or
execution concepts exists.

- [ ] **Step 2: Run a clean full build and test suite**

Run:

```sh
cmake --build build --clean-first --parallel
ctest --test-dir build --output-on-failure
```

Expected: every deterministic test passes; opt-in network smoke is skipped unless
enabled.

- [ ] **Step 3: Validate CLI help and configuration behavior**

Run:

```sh
./build/heatmap_viewer --help
./build/heatmap_viewer --venue kraken
```

Expected: help includes `--venue coinbase|kraken`; replay-mode venue use exits
nonzero with a clear `--venue requires --live` diagnostic.

- [ ] **Step 4: Review final diff for venue leakage and exactness**

Confirm Kraken native strings appear only in Kraken implementation/tests/docs,
Coinbase `SequenceTracker` remains session/feed owned, CRC tests use literal
expected values, and no product list is embedded in generic book or routing code.

- [ ] **Step 5: Commit any verification-only corrections**

If Step 1-4 required changes, stage only those exact files and commit:

```sh
git commit -m "Harden Kraken correctness verification"
```

If no corrections were necessary, do not create an empty commit.
