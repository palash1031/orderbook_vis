(function attachScannerModel(root, factory) {
  "use strict";

  const api = factory();

  if (typeof module === "object" && module.exports) {
    module.exports = api;
  }

  if (root) {
    root.DepthfieldScanner = api;
  }
}(typeof globalThis === "object" ? globalThis : this, () => {
  "use strict";

  const venues = ["coinbase", "kraken"];
  const statuses = new Set([
    "unsupported",
    "connecting",
    "waiting_for_snapshot",
    "live",
    "stale",
    "disconnected",
    "reconnecting",
  ]);

  function isRecord(value) {
    return Boolean(value) && typeof value === "object" && !Array.isArray(value);
  }

  function requireNullableNumber(value, label) {
    if (value !== null && !Number.isFinite(value)) {
      throw new Error(`${label} must be a finite number or null`);
    }
  }

  function validateHello(payload) {
    if (
      !isRecord(payload)
      || payload.type !== "scanner_hello"
      || payload.schema_version !== 1
    ) {
      throw new Error("Unsupported scanner stream metadata");
    }

    if (
      !Array.isArray(payload.venues)
      || payload.venues.length !== venues.length
      || payload.venues.some((venue, index) => venue !== venues[index])
    ) {
      throw new Error("Scanner stream venue list is invalid");
    }

    if (
      !Array.isArray(payload.products)
      || payload.products.length === 0
      || payload.products.some((product) => (
        typeof product !== "string"
        || !/^[A-Z0-9]+-[A-Z0-9]+$/.test(product)
      ))
      || new Set(payload.products).size !== payload.products.length
    ) {
      throw new Error("Scanner stream product list is invalid");
    }

    return payload;
  }

  function validateVenueState(value, venue) {
    if (!isRecord(value) || !statuses.has(value.status)) {
      throw new Error(`Scanner ${venue} status is invalid`);
    }

    requireNullableNumber(value.bid, `${venue} bid`);
    requireNullableNumber(value.bid_quantity, `${venue} bid quantity`);
    requireNullableNumber(value.ask, `${venue} ask`);
    requireNullableNumber(value.ask_quantity, `${venue} ask quantity`);
  }

  function validateConsolidated(value) {
    if (!isRecord(value)) {
      throw new Error("Scanner consolidated quote is invalid");
    }

    requireNullableNumber(value.best_bid, "consolidated bid");
    requireNullableNumber(value.best_bid_quantity, "consolidated bid quantity");
    requireNullableNumber(value.best_ask, "consolidated ask");
    requireNullableNumber(value.best_ask_quantity, "consolidated ask quantity");

    for (const side of ["bid", "ask"]) {
      const venue = value[`best_${side}_venue`];
      if (venue !== null && !venues.includes(venue)) {
        throw new Error(`Scanner best ${side} venue is invalid`);
      }
    }
  }

  function validateFragmentation(value) {
    if (value === null) return;
    if (!isRecord(value)) {
      throw new Error("Scanner fragmentation metrics are invalid");
    }

    for (const field of ["bid_bps", "ask_bps", "max_bps"]) {
      if (!Number.isFinite(value[field]) || value[field] < 0) {
        throw new Error(`Scanner fragmentation ${field} is invalid`);
      }
    }
  }

  class ScannerModel {
    constructor() {
      this.products = [];
      this.updates = new Map();
    }

    accept(payload) {
      if (payload?.type === "scanner_hello") {
        const hello = validateHello(payload);
        this.products = [...hello.products];
        this.updates.clear();
        return;
      }

      if (payload?.type !== "scanner_update") {
        throw new Error("Scanner stream sent an unknown message");
      }
      if (this.products.length === 0) {
        throw new Error("Scanner update arrived before metadata");
      }
      if (!this.products.includes(payload.product_id)) {
        throw new Error("Scanner update product is outside the configured universe");
      }
      if (!isRecord(payload.venues)) {
        throw new Error("Scanner venue quotes are invalid");
      }

      const venueNames = Object.keys(payload.venues).sort();
      if (venueNames.join(",") !== [...venues].sort().join(",")) {
        throw new Error("Scanner update has an unexpected venue");
      }

      for (const venue of venues) {
        validateVenueState(payload.venues[venue], venue);
      }
      validateConsolidated(payload.consolidated);
      validateFragmentation(payload.fragmentation);
      this.updates.set(payload.product_id, payload);
    }

    rows({ sortByDivergence = false } = {}) {
      const rows = this.products.map((productId, configuredIndex) => ({
        productId,
        configuredIndex,
        update: this.updates.get(productId) ?? null,
      }));

      if (!sortByDivergence) return rows;

      return rows.sort((left, right) => {
        const leftValue = left.update?.fragmentation?.max_bps;
        const rightValue = right.update?.fragmentation?.max_bps;
        const leftAvailable = Number.isFinite(leftValue);
        const rightAvailable = Number.isFinite(rightValue);

        if (leftAvailable !== rightAvailable) return leftAvailable ? -1 : 1;
        if (leftAvailable && leftValue !== rightValue) return rightValue - leftValue;
        return left.configuredIndex - right.configuredIndex;
      });
    }

    venueCell(productId, venue, side) {
      if (!this.products.includes(productId)) {
        throw new Error("Scanner cell product is outside the configured universe");
      }
      if (!venues.includes(venue) || (side !== "bid" && side !== "ask")) {
        throw new Error("Scanner cell selection is invalid");
      }

      const venueState = this.updates.get(productId)?.venues?.[venue];
      const status = venueState?.status ?? "connecting";

      if (status !== "live") {
        return { available: false, status, price: null, quantity: null };
      }

      const price = venueState[side];
      const quantity = venueState[`${side}_quantity`];
      const available = Number.isFinite(price) && Number.isFinite(quantity);
      return {
        available,
        status,
        price: available ? price : null,
        quantity: available ? quantity : null,
      };
    }
  }

  return { ScannerModel };
}));
