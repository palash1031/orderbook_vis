#include "coinbase_parser.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace
{
constexpr std::size_t synthetic_snapshot_levels = 1'000;

const std::string small_update_message = R"({
    "channel":"l2_data",
    "timestamp":"2026-08-28T00:00:00Z",
    "sequence_num":1,
    "events":[{
        "type":"update",
        "product_id":"BTC-USD",
        "updates":[
            {
                "side":"bid",
                "event_time":"2026-08-28T00:00:00Z",
                "price_level":"80000.01",
                "new_quantity":"1.5"
            },
            {
                "side":"offer",
                "event_time":"2026-08-28T00:00:00Z",
                "price_level":"80000.02",
                "new_quantity":"2.0"
            },
            {
                "side":"bid",
                "event_time":"2026-08-28T00:00:00Z",
                "price_level":"79999.99",
                "new_quantity":"0"
            }
        ]
    }]
})";

const std::string subscriptions_message = R"({
    "channel":"subscriptions",
    "timestamp":"2026-08-28T00:00:00Z",
    "sequence_num":3,
    "events":[{
        "subscriptions":{"level2":["BTC-USD"]}
    }]
})";

std::string make_synthetic_snapshot(std::size_t level_count)
{
    std::string message = R"({
        "channel":"l2_data",
        "timestamp":"2026-08-28T00:00:00Z",
        "sequence_num":0,
        "events":[{
            "type":"snapshot",
            "product_id":"BTC-USD",
            "updates":[
    )";
    message.reserve(message.size() + level_count * 140);

    for (std::size_t index = 0; index < level_count; ++index)
    {
        if (index != 0)
        {
            message += ',';
        }

        message += R"({"side":")";
        message += index % 2 == 0 ? "bid" : "offer";
        message += R"(","event_time":"2026-08-28T00:00:00Z",)";
        message += R"("price_level":")";
        message += std::to_string(70'000.0 + index * 0.01);
        message += R"(","new_quantity":")";
        message += std::to_string(1.0 + index * 0.001);
        message += R"("})";
    }

    message += R"(
            ]
        }]
    })";

    return message;
}

void parse_small_update_message(benchmark::State& state)
{
    for (auto _ : state)
    {
        ParsedCoinbaseMessage parsed =
            CoinbaseParser::parse_message(small_update_message);
        benchmark::DoNotOptimize(parsed);
    }

    state.SetItemsProcessed(state.iterations() * 3);
    state.SetBytesProcessed(
        state.iterations()
        * static_cast<std::int64_t>(small_update_message.size())
    );
}

void parse_non_l2_message(benchmark::State& state)
{
    for (auto _ : state)
    {
        ParsedCoinbaseMessage parsed =
            CoinbaseParser::parse_message(subscriptions_message);
        benchmark::DoNotOptimize(parsed);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(
        state.iterations()
        * static_cast<std::int64_t>(subscriptions_message.size())
    );
}

void parse_synthetic_snapshot(benchmark::State& state)
{
    const std::string snapshot =
        make_synthetic_snapshot(synthetic_snapshot_levels);

    for (auto _ : state)
    {
        ParsedCoinbaseMessage parsed =
            CoinbaseParser::parse_message(snapshot);
        benchmark::DoNotOptimize(parsed);
    }

    state.SetItemsProcessed(
        state.iterations()
        * static_cast<std::int64_t>(synthetic_snapshot_levels)
    );
    state.SetBytesProcessed(
        state.iterations()
        * static_cast<std::int64_t>(snapshot.size())
    );
}
}

BENCHMARK(parse_small_update_message)
    ->Name("Parser/ParseSmallUpdateMessage");
BENCHMARK(parse_non_l2_message)
    ->Name("Parser/ParseNonL2Message");
BENCHMARK(parse_synthetic_snapshot)
    ->Name("Parser/ParseSyntheticSnapshot1000");
