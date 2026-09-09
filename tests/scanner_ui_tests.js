"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { ScannerModel } = require("../web/scanner_model.js");

const products = ["BTC-USD", "UNI-USD", "HBAR-USD"];

function hello() {
  return {
    type: "scanner_hello",
    schema_version: 1,
    venues: ["coinbase", "kraken"],
    products,
  };
}

function update(productId, maxBps = null) {
  return {
    type: "scanner_update",
    product_id: productId,
    venues: {
      coinbase: {
        status: "live",
        bid: 100,
        bid_quantity: 2,
        ask: 101,
        ask_quantity: 3,
      },
      kraken: {
        status: "live",
        bid: 100.5,
        bid_quantity: 4,
        ask: 101.5,
        ask_quantity: 5,
      },
    },
    consolidated: {
      best_bid: 100.5,
      best_bid_quantity: 4,
      best_bid_venue: "kraken",
      best_ask: 101,
      best_ask_quantity: 3,
      best_ask_venue: "coinbase",
    },
    fragmentation: maxBps === null
      ? null
      : { bid_bps: maxBps / 2, ask_bps: maxBps, max_bps: maxBps },
  };
}

test("hello preserves configured order and exposes every market immediately", () => {
  const model = new ScannerModel();
  model.accept(hello());

  assert.deepEqual(
    model.rows().map((row) => row.productId),
    products,
  );
  assert.equal(model.rows()[1].update, null);
});

test("incremental updates replace only their own product", () => {
  const model = new ScannerModel();
  model.accept(hello());
  model.accept(update("UNI-USD", 7.25));

  const rows = model.rows();
  assert.equal(rows[0].update, null);
  assert.equal(rows[1].update.fragmentation.max_bps, 7.25);
  assert.equal(rows[2].update, null);
});

test("non-live venue cells never expose retained prices", () => {
  const model = new ScannerModel();
  model.accept(hello());
  const stale = update("BTC-USD", 4);
  stale.venues.kraken.status = "stale";
  stale.venues.kraken.bid = 999;
  stale.venues.kraken.bid_quantity = 100;
  model.accept(stale);

  assert.deepEqual(model.venueCell("BTC-USD", "kraken", "bid"), {
    available: false,
    status: "stale",
    price: null,
    quantity: null,
  });
  assert.deepEqual(model.venueCell("BTC-USD", "coinbase", "bid"), {
    available: true,
    status: "live",
    price: 100,
    quantity: 2,
  });
});

test("optional divergence sorting is descending and stable", () => {
  const model = new ScannerModel();
  model.accept(hello());
  model.accept(update("BTC-USD", 1.5));
  model.accept(update("UNI-USD", 8));

  assert.deepEqual(
    model.rows({ sortByDivergence: true }).map((row) => row.productId),
    ["UNI-USD", "BTC-USD", "HBAR-USD"],
  );
  assert.deepEqual(
    model.rows().map((row) => row.productId),
    products,
  );
});

test("invalid messages and unknown products reject cleanly", () => {
  const model = new ScannerModel();
  assert.throws(() => model.accept({ ...hello(), schema_version: 2 }));

  model.accept(hello());
  assert.throws(() => model.accept(update("SOL-USD", 1)));

  const invalidStatus = update("BTC-USD", 1);
  invalidStatus.venues.coinbase.status = "maybe";
  assert.throws(() => model.accept(invalidStatus));
});
