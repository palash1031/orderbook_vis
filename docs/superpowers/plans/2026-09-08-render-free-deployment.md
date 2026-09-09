# Render Free Deployment Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a free, public, read-only Render deployment of the existing live heatmap viewer without changing order-book or exchange correctness.

**Architecture:** Extend the existing pure viewer configuration parser, enforce browser mutation policy inside `LiveMarketService`, and make the HTTP listener consume the parsed endpoint. Keep reconnect behavior in the browser and deployment details in Docker/Render configuration so exchange adapters remain untouched.

**Tech Stack:** C++20, Boost.Asio/Beast, Boost.JSON, OpenSSL, GoogleTest, browser JavaScript, Docker, Render Blueprint.

**Spec:** `docs/superpowers/specs/2026-09-08-render-free-deployment-design.md`

## Global Constraints

- Preserve recorder, replay, Coinbase sequence validation, Kraken checksum validation, live heatmap reconstruction, recovery, and heatmap math.
- Local defaults remain `127.0.0.1:8080`; only an explicit bind flag exposes the viewer beyond loopback.
- Explicit `--port` takes precedence over `PORT`; both use the inclusive range `1..65535`.
- Public-demo mode streams normally but cannot mutate the process-wide live product.
- The browser maintains at most one active socket and one scheduled reconnect, with delay capped at 10 seconds.
- Render uses one Docker Free Web Service and `/health`; no database, credentials, private API, or recorder process is added.
- Every C++ production behavior begins with a witnessed failing test.

---

### Task 1: Viewer endpoint and environment configuration

**Files:**
- Modify: `include/viewer_config.hpp`
- Modify: `src/viewer_config.cpp`
- Modify: `tests/viewer_config_tests.cpp`
- Modify: `src/heatmap_viewer.cpp`

**Interfaces:**
- Produces: `ViewerOptions::bind_address`, `ViewerOptions::public_demo`, and `parse_viewer_options(arguments, default_web_root, environment_port)`.
- Consumes: an optional `std::string_view` containing the process `PORT` value; no test mutates global environment state.

- [ ] **Step 1: Add failing endpoint precedence tests**

Add literal assertions for the local defaults, `--bind 0.0.0.0`, `PORT=10000`,
`--port 8081` overriding `PORT=10000`, valid CLI overriding malformed `PORT`,
invalid/zero/out-of-range environment values, malformed bind values, and
`--public-demo`.

```cpp
TEST(ViewerConfigTest, UsesEnvironmentPortWhenCliPortIsAbsent)
{
    constexpr std::array<std::string_view, 1> arguments{"--live"};
    const ViewerOptions options = parse_viewer_options(
        arguments,
        "web",
        "10000"
    );
    EXPECT_EQ(options.port, 10'000);
}

TEST(ViewerConfigTest, CliPortOverridesMalformedEnvironmentPort)
{
    constexpr std::array<std::string_view, 3> arguments{
        "--live", "--port", "8081"
    };
    const ViewerOptions options = parse_viewer_options(
        arguments,
        "web",
        "not-a-port"
    );
    EXPECT_EQ(options.port, 8'081);
}
```

- [ ] **Step 2: Build and witness the expected failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R '^ViewerConfigTest\.'
```

Expected: compilation fails because the environment-port overload and new
fields do not exist.

- [ ] **Step 3: Implement minimal parsing and listener wiring**

Extend the public shape to:

```cpp
struct ViewerOptions
{
    std::filesystem::path heatmap_path = "heatmap.json";
    std::filesystem::path web_root = "web";
    std::string product_id = "BTC-USD";
    HeatmapConfig live_heatmap_config;
    Venue venue = Venue::Coinbase;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 8080;
    bool live = false;
    bool show_help = false;
    bool public_demo = false;
};

ViewerOptions parse_viewer_options(
    std::span<const std::string_view> arguments,
    std::filesystem::path default_web_root = "web",
    std::optional<std::string_view> environment_port = std::nullopt
);
```

Track whether CLI supplied a port, parse `environment_port` only after all CLI
arguments when it did not, and validate `--bind` with
`boost::asio::ip::make_address`. In `main`, wrap `std::getenv("PORT")` as the
optional input. Build the server endpoint with `make_address(options.bind_address)`
and update usage/startup output.

- [ ] **Step 4: Verify focused configuration behavior**

Run:

```sh
cmake --build build --target orderbook_tests heatmap_viewer --parallel
ctest --test-dir build --output-on-failure -R '^ViewerConfigTest\.'
```

Expected: every viewer configuration test passes.

- [ ] **Step 5: Commit endpoint configuration**

```sh
git add include/viewer_config.hpp src/viewer_config.cpp tests/viewer_config_tests.cpp src/heatmap_viewer.cpp
git commit -m "Add deployable viewer endpoint configuration"
```

---

### Task 2: Read-only public live controls

**Files:**
- Modify: `include/live_source.hpp`
- Modify: `src/live_source.cpp`
- Modify: `src/heatmap_viewer.cpp`
- Modify: `tests/live_source_tests.cpp`

**Interfaces:**
- Produces: `enum class LiveControlAccess { Interactive, ReadOnly }` and a final optional `LiveMarketService` constructor parameter.
- Preserves: direct internal `switch_product()` and all interactive-mode controls.

- [ ] **Step 1: Add a failing mutation-boundary test**

Construct a service with `LiveControlAccess::ReadOnly`, drain its initial
subscriber state, call the real `apply_control()` with a valid switch command,
and assert that it throws, the product remains `SOL-USD`, and no reset/state
messages were published.

```cpp
EXPECT_THROW(
    service.apply_control(
        R"({"action":"switch_product","product_id":"ETH-USD"})"
    ),
    std::invalid_argument
);
EXPECT_EQ(service.product_id(), "SOL-USD");
EXPECT_TRUE(drain(client).empty());
```

- [ ] **Step 2: Run the focused test and witness failure**

Run:

```sh
cmake --build build --target orderbook_tests --parallel
ctest --test-dir build --output-on-failure -R '^LiveMarketServiceTest\.'
```

Expected: compilation fails because `LiveControlAccess` does not exist.

- [ ] **Step 3: Implement read-only enforcement**

Store the access value in `LiveMarketService`. At the start of
`apply_control()`, throw `std::invalid_argument` for read-only clients before
parsing or switching. In `load_state`, pass `ReadOnly` only when
`options.public_demo` is true; otherwise retain `Interactive`.

- [ ] **Step 4: Verify interactive and read-only controls together**

Run:

```sh
cmake --build build --target orderbook_tests heatmap_viewer --parallel
ctest --test-dir build --output-on-failure -R '^LiveMarketServiceTest\.'
```

Expected: existing switch tests and the new rejection test pass.

- [ ] **Step 5: Commit the access boundary**

```sh
git add include/live_source.hpp src/live_source.cpp src/heatmap_viewer.cpp tests/live_source_tests.cpp
git commit -m "Make public live controls read only"
```

---

### Task 3: Browser reconnection and recruiter identity

**Files:**
- Modify: `web/app.js`
- Modify: `web/index.html`
- Modify: `web/styles.css`

**Interfaces:**
- Produces: `scheduleReconnect()` and an idempotent `connectStream()` using state-owned socket, timer, and attempt counters.
- Consumes: the existing `/ws/heatmap` same-origin endpoint and existing retained-stream protocol.

- [ ] **Step 1: Add the reconnect state machine**

Add `reconnectTimer` and `reconnectAttempt` to browser state. Guard
`connectStream()` when a socket is already connecting/open. On unexpected
close, clear only that socket, disable backend controls, show `Reconnecting…`,
and schedule `min(500 * 2^attempt, 10000)` milliseconds. Clear the timer and
reset attempts on `open`. Mark the protocol-error close as deliberate so an
incompatible stream does not loop.

- [ ] **Step 2: Preserve local controls and add minimal identity copy**

Keep intensity, price window, side, midpoint, cursor, and reset handlers
unchanged. Add `C++20 MULTI-VENUE MARKET DATA ENGINE` below `DEPTHFIELD`, retain
the dynamic stream label, and add a small external link to
`https://github.com/palash1031/orderbook_vis`.

- [ ] **Step 3: Validate browser syntax**

Run:

```sh
node --check web/app.js
```

Expected: exit code 0 with no syntax error.

- [ ] **Step 4: Commit browser deployment resilience**

```sh
git add web/app.js web/index.html web/styles.css
git commit -m "Reconnect public viewer WebSockets"
```

---

### Task 4: Production container and free Render Blueprint

**Files:**
- Create: `Dockerfile`
- Create: `.dockerignore`
- Create: `render.yaml`
- Modify: `CMakeLists.txt`
- Modify: `README.md`

**Interfaces:**
- Produces: a non-root `/app/heatmap_viewer` image with `/app/web` assets and a default public Coinbase `UNI-USD` command.
- Consumes: Render-provided `PORT`, Docker CMD overrides, and `/health`.

- [ ] **Step 1: Add container and Blueprint configuration**

Use a Debian Bookworm builder with CMake, a C++ compiler, Boost.JSON headers,
OpenSSL headers, and certificates. Configure Release with `BUILD_TESTING=OFF`.
Use a Debian Bookworm slim runtime with only certificates and corresponding
Boost.JSON/OpenSSL runtime packages, then copy the viewer and web assets.

Set the Docker default arguments to:

```text
--live --venue coinbase --product UNI-USD --public-demo --bind 0.0.0.0 --web-root /app/web
```

Define one Blueprint service with `runtime: docker`, `plan: free`, and
`healthCheckPath: /health`. Guard `CMP0167` with `if(POLICY CMP0167)` so the
documented CMake 3.20 minimum and Bookworm CMake remain valid.

- [ ] **Step 2: Document free deployment and local Docker proof**

Document the exact build/run command, Render Dashboard/Blueprint flow, free
cold-start behavior, `PORT` precedence, loopback-safe local default, and the
fact that public-demo prevents global live product switching.

- [ ] **Step 3: Build the image**

Run:

```sh
docker build -t depthfield .
```

Expected: the Release image builds and ends with `/app/heatmap_viewer` plus the
three `/app/web` assets.

- [ ] **Step 4: Smoke-test HTTP and WebSocket behavior**

Run the container mapped to local port 8080 with the documented override.
Verify `GET /health` returns HTTP 200, the root page loads, and a WebSocket
client receives live Coinbase state. Send a valid `switch_product` command and
verify an error arrives while the stream remains on `UNI-USD`.

- [ ] **Step 5: Commit packaging and deployment documentation**

```sh
git add Dockerfile .dockerignore render.yaml CMakeLists.txt README.md
git commit -m "Package free Render deployment"
```

---

### Task 5: Full regression verification

**Files:**
- Review only: all files changed by Tasks 1–4

**Interfaces:**
- Verifies: no changes are needed in order-book, Coinbase, Kraken, replay, or heatmap calculation code.

- [ ] **Step 1: Configure and build from the branch**

Run:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Expected: every target builds without warnings promoted to errors.

- [ ] **Step 2: Run the deterministic suite**

Run:

```sh
ctest --test-dir build --output-on-failure
```

Expected: all deterministic tests pass; the opt-in public Kraken smoke test is
reported skipped.

- [ ] **Step 3: Check patch integrity and scope**

Run:

```sh
git diff main...HEAD --check
git status --short --branch
```

Expected: no whitespace errors, no uncommitted files, and no exchange adapter,
book reconstruction, replay, or heatmap-math files in the patch.
