"use strict";

const canvas = document.querySelector("#heatmap");
const frame = document.querySelector("#chart-frame");
const context = canvas.getContext("2d", { alpha: false });
const message = document.querySelector("#chart-message");
const tooltip = document.querySelector("#tooltip");
const connection = document.querySelector("#connection-state");

const controls = {
  intensity: document.querySelector("#intensity"),
  intensityValue: document.querySelector("#intensity-value"),
  priceWindow: document.querySelector("#price-window"),
  priceWindowValue: document.querySelector("#price-window-value"),
  midline: document.querySelector("#midline"),
  side: document.querySelector("#side-control"),
  reset: document.querySelector("#reset-view"),
};

const cursorFields = {
  time: document.querySelector("#cursor-time"),
  price: document.querySelector("#cursor-price"),
  bid: document.querySelector("#cursor-bid"),
  ask: document.querySelector("#cursor-ask"),
};

const state = {
  data: null,
  side: "both",
  intensity: 1,
  priceWindow: 100,
  showMidline: true,
  hover: null,
  geometry: null,
  renderQueued: false,
};

const palette = {
  background: "#080c12",
  grid: "rgba(126, 143, 160, 0.14)",
  axis: "#748190",
  bid: [39, 183, 255],
  ask: [255, 101, 95],
  mid: "rgba(240, 247, 251, 0.9)",
  crosshair: "rgba(238, 244, 248, 0.34)",
};

function formatPrice(value) {
  if (!Number.isFinite(value)) return "—";
  const decimals = state.data?.config.price_bin_size < 1 ? 2 : 0;
  return value.toLocaleString("en-US", {
    minimumFractionDigits: decimals,
    maximumFractionDigits: Math.max(decimals, 2),
  });
}

function formatQuantity(value) {
  if (!Number.isFinite(value)) return "—";
  if (value === 0) return "0";
  if (value < 0.001) return value.toExponential(2);
  return value.toLocaleString("en-US", { maximumFractionDigits: 4 });
}

function formatDuration(milliseconds) {
  if (milliseconds < 1_000) return `${Math.max(0, milliseconds).toFixed(0)} ms`;
  if (milliseconds < 60_000) return `${(milliseconds / 1_000).toFixed(1)} sec`;
  return `${(milliseconds / 60_000).toFixed(1)} min`;
}

function formatTime(timestamp) {
  return new Intl.DateTimeFormat("en-US", {
    timeZone: "UTC",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    fractionalSecondDigits: 3,
    hour12: false,
  }).format(new Date(timestamp));
}

function validateDocument(document) {
  if (document?.schema_version !== 1) {
    throw new Error("Unsupported heatmap schema");
  }
  if (!Array.isArray(document.columns) || document.columns.length === 0) {
    throw new Error("Heatmap contains no replay columns");
  }

  const count = document.config?.price_bin_count;
  if (!Number.isInteger(count) || count <= 0) {
    throw new Error("Heatmap price-bin configuration is invalid");
  }

  for (const column of document.columns) {
    if (
      !Array.isArray(column.bids)
      || !Array.isArray(column.asks)
      || column.bids.length !== count
      || column.asks.length !== count
    ) {
      throw new Error("Heatmap column dimensions do not match configuration");
    }
  }

  return document;
}

function summarize(document) {
  const columns = document.columns;
  const first = columns[0];
  const last = columns.at(-1);
  let peak = 0;

  for (const column of columns) {
    for (let index = 0; index < column.bids.length; index += 1) {
      peak = Math.max(peak, column.bids[index], column.asks[index]);
    }
  }

  document.summary = {
    peak,
    start: first.timestamp_ms,
    end: last.timestamp_ms,
    duration: last.timestamp_ms - first.timestamp_ms,
  };
}

function updateSummary() {
  const { data } = state;
  const latest = data.columns.at(-1);

  document.querySelector("#product-id").textContent = data.product_id || "UNKNOWN";
  document.querySelector("#last-mid").textContent = formatPrice(latest.mid_price);
  document.querySelector("#window-duration").textContent = formatDuration(data.summary.duration);
  document.querySelector("#column-count").textContent = data.columns.length.toLocaleString();
  document.querySelector("#price-bin").textContent = `$${formatPrice(data.config.price_bin_size)}`;
  document.querySelector("#peak-depth").textContent = formatQuantity(data.summary.peak);
  document.querySelector("#replay-range").textContent = `${formatTime(data.summary.start)} — ${formatTime(data.summary.end)} UTC`;

  connection.className = "connection ready";
  connection.querySelector("span:last-child").textContent = "Replay loaded";
  message.hidden = true;
}

function computeGeometry(width, height) {
  const margins = {
    top: 18,
    right: 18,
    bottom: 38,
    left: width < 560 ? 58 : 76,
  };
  const plot = {
    x: margins.left,
    y: margins.top,
    width: Math.max(1, width - margins.left - margins.right),
    height: Math.max(1, height - margins.top - margins.bottom),
  };

  const columns = state.data.columns;
  const binSize = state.data.config.price_bin_size;
  let fullMin = Infinity;
  let fullMax = -Infinity;

  for (const column of columns) {
    fullMin = Math.min(fullMin, column.first_price);
    fullMax = Math.max(fullMax, column.first_price + column.bids.length * binSize);
  }

  const latestMid = columns.at(-1).mid_price;
  const fullSpan = Math.max(binSize, fullMax - fullMin);
  const visibleSpan = fullSpan * (state.priceWindow / 100);
  let minPrice = fullMin;
  let maxPrice = fullMax;

  if (state.priceWindow < 100) {
    minPrice = latestMid - visibleSpan / 2;
    maxPrice = latestMid + visibleSpan / 2;
  }

  return {
    width,
    height,
    plot,
    minPrice,
    maxPrice,
    priceSpan: maxPrice - minPrice,
    cellWidth: plot.width / columns.length,
  };
}

function priceToY(price, geometry) {
  return geometry.plot.y
    + ((geometry.maxPrice - price) / geometry.priceSpan) * geometry.plot.height;
}

function drawGrid(geometry) {
  const { plot } = geometry;
  context.fillStyle = palette.background;
  context.fillRect(0, 0, geometry.width, geometry.height);
  context.strokeStyle = palette.grid;
  context.lineWidth = 1;

  context.beginPath();
  for (let index = 0; index <= 5; index += 1) {
    const y = plot.y + (plot.height * index) / 5;
    context.moveTo(plot.x, Math.round(y) + 0.5);
    context.lineTo(plot.x + plot.width, Math.round(y) + 0.5);
  }
  for (let index = 0; index <= 6; index += 1) {
    const x = plot.x + (plot.width * index) / 6;
    context.moveTo(Math.round(x) + 0.5, plot.y);
    context.lineTo(Math.round(x) + 0.5, plot.y + plot.height);
  }
  context.stroke();
}

function depthAlpha(quantity) {
  if (quantity <= 0) return 0;
  const peak = Math.max(state.data.summary.peak, Number.EPSILON);
  const scaled = Math.log1p(quantity * state.intensity) / Math.log1p(peak * state.intensity);
  return Math.min(1, 0.08 + scaled * 0.92);
}

function drawDepth(geometry) {
  const { columns } = state.data;
  const binSize = state.data.config.price_bin_size;
  const cellHeight = Math.max(1, (binSize / geometry.priceSpan) * geometry.plot.height + 0.35);
  const cellWidth = Math.max(1, geometry.cellWidth + 0.45);

  for (let columnIndex = 0; columnIndex < columns.length; columnIndex += 1) {
    const column = columns[columnIndex];
    const x = geometry.plot.x + columnIndex * geometry.cellWidth;

    for (let binIndex = 0; binIndex < column.bids.length; binIndex += 1) {
      const price = column.first_price + (binIndex + 0.5) * binSize;
      if (price < geometry.minPrice || price > geometry.maxPrice) continue;

      const y = priceToY(price, geometry) - cellHeight / 2;
      const bid = state.side !== "ask" ? column.bids[binIndex] : 0;
      const ask = state.side !== "bid" ? column.asks[binIndex] : 0;

      if (bid > 0) {
        context.fillStyle = `rgba(${palette.bid.join(",")},${depthAlpha(bid)})`;
        context.fillRect(x, y, cellWidth, cellHeight);
      }
      if (ask > 0) {
        context.fillStyle = `rgba(${palette.ask.join(",")},${depthAlpha(ask)})`;
        context.fillRect(x, y, cellWidth, cellHeight);
      }
    }
  }
}

function drawMidline(geometry) {
  if (!state.showMidline) return;

  context.beginPath();
  context.strokeStyle = palette.mid;
  context.lineWidth = 1.35;
  context.shadowColor = "rgba(255,255,255,0.42)";
  context.shadowBlur = 5;

  state.data.columns.forEach((column, index) => {
    const x = geometry.plot.x + (index + 0.5) * geometry.cellWidth;
    const y = priceToY(column.mid_price, geometry);
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  });

  context.stroke();
  context.shadowBlur = 0;
}

function drawAxes(geometry) {
  const { plot } = geometry;
  context.fillStyle = palette.axis;
  context.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
  context.textBaseline = "middle";
  context.textAlign = "right";

  for (let index = 0; index <= 5; index += 1) {
    const ratio = index / 5;
    const price = geometry.maxPrice - ratio * geometry.priceSpan;
    const y = plot.y + ratio * plot.height;
    context.fillText(formatPrice(price), plot.x - 9, y);
  }

  context.textBaseline = "top";
  context.textAlign = "center";
  const firstTime = state.data.columns[0].timestamp_ms;
  const lastTime = state.data.columns.at(-1).timestamp_ms;

  for (let index = 0; index <= 3; index += 1) {
    const ratio = index / 3;
    const timestamp = firstTime + (lastTime - firstTime) * ratio;
    const x = plot.x + plot.width * ratio;
    context.fillText(formatTime(timestamp), x, plot.y + plot.height + 13);
  }
}

function drawHover(geometry) {
  if (!state.hover) return;
  const { x, y } = state.hover;
  if (
    x < geometry.plot.x
    || x > geometry.plot.x + geometry.plot.width
    || y < geometry.plot.y
    || y > geometry.plot.y + geometry.plot.height
  ) return;

  context.strokeStyle = palette.crosshair;
  context.lineWidth = 1;
  context.setLineDash([3, 4]);
  context.beginPath();
  context.moveTo(x, geometry.plot.y);
  context.lineTo(x, geometry.plot.y + geometry.plot.height);
  context.moveTo(geometry.plot.x, y);
  context.lineTo(geometry.plot.x + geometry.plot.width, y);
  context.stroke();
  context.setLineDash([]);
}

function render() {
  state.renderQueued = false;
  if (!state.data || !context) return;

  const bounds = frame.getBoundingClientRect();
  const ratio = Math.min(window.devicePixelRatio || 1, 2);
  const width = Math.max(1, Math.floor(bounds.width));
  const height = Math.max(1, Math.floor(bounds.height));

  if (canvas.width !== width * ratio || canvas.height !== height * ratio) {
    canvas.width = width * ratio;
    canvas.height = height * ratio;
    canvas.style.width = `${width}px`;
    canvas.style.height = `${height}px`;
  }

  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  const geometry = computeGeometry(width, height);
  state.geometry = geometry;
  drawGrid(geometry);
  context.save();
  context.beginPath();
  context.rect(geometry.plot.x, geometry.plot.y, geometry.plot.width, geometry.plot.height);
  context.clip();
  drawDepth(geometry);
  drawMidline(geometry);
  drawHover(geometry);
  context.restore();
  drawAxes(geometry);
}

function queueRender() {
  if (state.renderQueued) return;
  state.renderQueued = true;
  requestAnimationFrame(render);
}

function readingAt(x, y) {
  const geometry = state.geometry;
  if (!geometry) return null;
  if (
    x < geometry.plot.x
    || x > geometry.plot.x + geometry.plot.width
    || y < geometry.plot.y
    || y > geometry.plot.y + geometry.plot.height
  ) return null;

  const columnIndex = Math.min(
    state.data.columns.length - 1,
    Math.max(0, Math.floor((x - geometry.plot.x) / geometry.cellWidth)),
  );
  const column = state.data.columns[columnIndex];
  const price = geometry.maxPrice
    - ((y - geometry.plot.y) / geometry.plot.height) * geometry.priceSpan;
  const binIndex = Math.floor(
    (price - column.first_price) / state.data.config.price_bin_size,
  );

  if (binIndex < 0 || binIndex >= column.bids.length) return null;

  return {
    column,
    price: column.first_price + binIndex * state.data.config.price_bin_size,
    bid: column.bids[binIndex],
    ask: column.asks[binIndex],
  };
}

function clearReading() {
  state.hover = null;
  tooltip.hidden = true;
  Object.values(cursorFields).forEach((field) => { field.textContent = "—"; });
  queueRender();
}

function updateReading(event) {
  const bounds = canvas.getBoundingClientRect();
  const x = event.clientX - bounds.left;
  const y = event.clientY - bounds.top;
  const reading = readingAt(x, y);

  if (!reading) {
    clearReading();
    return;
  }

  state.hover = { x, y };
  cursorFields.time.textContent = `${formatTime(reading.column.timestamp_ms)} UTC`;
  cursorFields.price.textContent = `$${formatPrice(reading.price)}`;
  cursorFields.bid.textContent = formatQuantity(reading.bid);
  cursorFields.ask.textContent = formatQuantity(reading.ask);

  tooltip.innerHTML = [
    `<div><b>Time</b>${formatTime(reading.column.timestamp_ms)} UTC</div>`,
    `<div><b>Price</b>$${formatPrice(reading.price)}</div>`,
    `<div class="bid-value"><b>Bid</b>${formatQuantity(reading.bid)}</div>`,
    `<div class="ask-value"><b>Ask</b>${formatQuantity(reading.ask)}</div>`,
  ].join("");
  tooltip.hidden = false;

  const tooltipX = Math.min(bounds.width - 178, Math.max(8, x + 14));
  const tooltipY = Math.min(bounds.height - 112, Math.max(8, y + 14));
  tooltip.style.transform = `translate(${tooltipX}px, ${tooltipY}px)`;
  queueRender();
}

function bindControls() {
  controls.intensity.addEventListener("input", () => {
    state.intensity = Number(controls.intensity.value);
    controls.intensityValue.value = `${state.intensity.toFixed(2).replace(/0$/, "")}×`;
    queueRender();
  });

  controls.priceWindow.addEventListener("input", () => {
    state.priceWindow = Number(controls.priceWindow.value);
    controls.priceWindowValue.value = `${state.priceWindow}%`;
    clearReading();
  });

  controls.midline.addEventListener("change", () => {
    state.showMidline = controls.midline.checked;
    queueRender();
  });

  controls.side.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-side]");
    if (!button) return;
    state.side = button.dataset.side;
    controls.side.querySelectorAll("button").forEach((candidate) => {
      const active = candidate === button;
      candidate.classList.toggle("active", active);
      candidate.setAttribute("aria-pressed", String(active));
    });
    queueRender();
  });

  controls.reset.addEventListener("click", () => {
    state.side = "both";
    state.intensity = 1;
    state.priceWindow = 100;
    state.showMidline = true;
    controls.intensity.value = "1";
    controls.intensityValue.value = "1.0×";
    controls.priceWindow.value = "100";
    controls.priceWindowValue.value = "100%";
    controls.midline.checked = true;
    controls.side.querySelectorAll("button").forEach((button) => {
      const active = button.dataset.side === "both";
      button.classList.toggle("active", active);
      button.setAttribute("aria-pressed", String(active));
    });
    clearReading();
  });

  canvas.addEventListener("pointermove", updateReading);
  canvas.addEventListener("pointerleave", clearReading);
  canvas.addEventListener("blur", clearReading);
  new ResizeObserver(queueRender).observe(frame);
}

async function loadHeatmap() {
  try {
    const response = await fetch("/api/heatmap", { cache: "no-store" });
    if (!response.ok) throw new Error(`Heatmap request failed (${response.status})`);
    state.data = validateDocument(await response.json());
    summarize(state.data);
    updateSummary();
    bindControls();
    queueRender();
  } catch (error) {
    connection.className = "connection error";
    connection.querySelector("span:last-child").textContent = "Replay unavailable";
    message.textContent = error instanceof Error ? error.message : "Unable to load heatmap";
  }
}

loadHeatmap();
