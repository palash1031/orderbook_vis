FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        libboost-json1.81-dev \
        libssl-dev \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_BENCHMARKS=OFF \
        -DBUILD_TESTING=OFF \
    && cmake --build build --target heatmap_viewer --parallel

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libboost-json1.81.0 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 depthfield \
    && useradd --system --uid 10001 --gid depthfield --home-dir /app depthfield

WORKDIR /app
COPY --from=builder --chown=depthfield:depthfield /src/build/heatmap_viewer ./heatmap_viewer
COPY --chown=depthfield:depthfield web ./web

USER depthfield
ENV PORT=10000
EXPOSE 10000

CMD ["./heatmap_viewer", "--scanner", "--public-demo", "--bind", "0.0.0.0", "--web-root", "/app/web"]
