#include "kraken_universe_session.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace json = boost::json;

namespace
{
struct WireState
{
    std::deque<std::string> reads;
    std::vector<std::string> writes;
};

class ScriptedKrakenUniverseWire final : public KrakenWire
{
public:
    explicit ScriptedKrakenUniverseWire(std::shared_ptr<WireState> state)
        : state_(std::move(state))
    {
    }

    void write(std::string_view message) override
    {
        state_->writes.emplace_back(message);
    }

    std::string read() override
    {
        if (state_->reads.empty())
        {
            throw std::runtime_error("scripted Kraken wire exhausted");
        }

        std::string message = std::move(state_->reads.front());
        state_->reads.pop_front();
        return message;
    }

private:
    std::shared_ptr<WireState> state_;
};

std::unique_ptr<KrakenWire> scripted_wire(
    const std::shared_ptr<WireState>& state)
{
    return std::make_unique<ScriptedKrakenUniverseWire>(state);
}

std::vector<std::string> read_fixture_lines(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path()
        / "fixtures"
        / name;
    std::ifstream input(path);

    if (!input)
    {
        throw std::runtime_error("Failed to open fixture: " + path.string());
    }

    std::vector<std::string> lines;
    std::string line;

    while (std::getline(input, line))
    {
        if (!line.empty())
        {
            lines.push_back(std::move(line));
        }
    }

    return lines;
}

std::shared_ptr<WireState> wire_state(
    const std::vector<std::string>& reads)
{
    auto state = std::make_shared<WireState>();
    state->reads.assign(reads.begin(), reads.end());
    return state;
}

MarketUniverse four_product_universe()
{
    return MarketUniverse({
        Product("BTC", "USD"),
        Product("UNI", "USD"),
        Product("ETH", "USD"),
        Product("HBAR", "USD")
    });
}

MarketUniverse btc_uni_universe()
{
    return MarketUniverse({Product("BTC", "USD"), Product("UNI", "USD")});
}

std::vector<VenueMarketStatusEvent> read_status_events(
    VenueUniverseSession& session,
    std::size_t count)
{
    std::vector<VenueMarketStatusEvent> statuses;

    for (std::size_t reads = 0; reads < 100 && statuses.size() < count; ++reads)
    {
        VenueSessionEvent event = session.read_event();

        if (const auto* status = std::get_if<VenueMarketStatusEvent>(&event))
        {
            statuses.push_back(*status);
        }
    }

    return statuses;
}

std::vector<TrustedBookEvent> read_trusted_events(
    VenueUniverseSession& session,
    std::size_t count)
{
    std::vector<TrustedBookEvent> trusted;

    for (std::size_t reads = 0; reads < 100 && trusted.size() < count; ++reads)
    {
        VenueSessionEvent event = session.read_event();

        if (const auto* book = std::get_if<TrustedBookEvent>(&event))
        {
            trusted.push_back(*book);
        }
    }

    return trusted;
}

std::string corrupt_uni_update()
{
    return R"({"channel":"book","type":"update","data":[{"symbol":"UNI/USD","bids":[{"price":"4.50","qty":"9"}],"asks":[],"checksum":0,"timestamp":"2026-09-08T12:00:00.000000003Z"}]})";
}

std::vector<std::string> first_six_fixture_frames()
{
    std::vector<std::string> frames = read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    );
    frames.resize(6);
    return frames;
}
}

TEST(KrakenUniverseSessionTest, DiscoversAndDispatchesInterleavedProducts)
{
    const auto state = wire_state(read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    ));
    KrakenUniverseSession session(
        four_product_universe(),
        scripted_wire(state)
    );

    ASSERT_EQ(state->writes.size(), 2U);
    const json::object discovery = json::parse(state->writes[0]).as_object();
    EXPECT_EQ(
        discovery.at("params").as_object().at("channel").as_string(),
        "instrument"
    );
    const json::object subscription = json::parse(state->writes[1]).as_object();
    const json::array& native_symbols = subscription.at("params")
        .as_object().at("symbol").as_array();
    ASSERT_EQ(native_symbols.size(), 2U);
    EXPECT_EQ(native_symbols[0].as_string(), "XBT/USD");
    EXPECT_EQ(native_symbols[1].as_string(), "UNI/USD");

    const std::vector<VenueMarketStatusEvent> statuses =
        read_status_events(session, 8);
    ASSERT_EQ(statuses.size(), 8U);
    EXPECT_EQ(statuses[0].market.product, Product("BTC", "USD"));
    EXPECT_EQ(statuses[0].status, VenueMarketStatus::Connecting);
    EXPECT_EQ(statuses[4].market.product, Product("ETH", "USD"));
    EXPECT_EQ(statuses[4].status, VenueMarketStatus::Unsupported);
    EXPECT_EQ(statuses[5].market.product, Product("HBAR", "USD"));
    EXPECT_EQ(statuses[5].status, VenueMarketStatus::Unsupported);
    EXPECT_EQ(statuses[6].market.product, Product("BTC", "USD"));
    EXPECT_EQ(statuses[6].status, VenueMarketStatus::WaitingForSnapshot);
    EXPECT_EQ(statuses[7].market.product, Product("UNI", "USD"));
    EXPECT_EQ(statuses[7].status, VenueMarketStatus::WaitingForSnapshot);

    const std::vector<TrustedBookEvent> events = read_trusted_events(session, 4);
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].market, (MarketKey{Venue::Kraken, Product("BTC", "USD")}));
    EXPECT_EQ(events[0].type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events[0].book->best_bid(), 100.0);
    EXPECT_EQ(events[1].market, (MarketKey{Venue::Kraken, Product("UNI", "USD")}));
    EXPECT_EQ(events[1].type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events[1].book->best_ask(), 4.51);
    EXPECT_EQ(events[2].market.product, Product("UNI", "USD"));
    EXPECT_EQ(events[2].type, TrustedBookEventType::Update);
    EXPECT_EQ(events[2].book->best_ask(), 4.52);
    EXPECT_EQ(events[3].market.product, Product("BTC", "USD"));
    EXPECT_EQ(events[3].type, TrustedBookEventType::Update);
    EXPECT_EQ(events[3].book->bids().at(100.0), 5.0);
}

TEST(KrakenUniverseSessionTest, UnknownNativeSymbolIsProtocolFailure)
{
    std::vector<std::string> frames = read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    );
    frames.resize(3);
    frames.push_back(R"({"channel":"book","type":"snapshot","data":[{"symbol":"DOGE/USD","bids":[{"price":"1","qty":"1"}],"asks":[{"price":"2","qty":"1"}],"checksum":0,"timestamp":"2026-09-08T12:00:00Z"}]})");
    const auto state = wire_state(frames);
    KrakenUniverseSession session(
        MarketUniverse({Product("BTC", "USD")}),
        scripted_wire(state)
    );

    ASSERT_EQ(read_status_events(session, 2).size(), 2U);
    EXPECT_THROW(session.read_event(), std::runtime_error);
}

TEST(KrakenUniverseSessionTest, ChecksumFailureInvalidatesBeforeReconnect)
{
    std::vector<std::string> frames = first_six_fixture_frames();
    frames.push_back(corrupt_uni_update());
    frames.push_back(read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    ).at(7));
    const auto state = wire_state(frames);
    KrakenUniverseSession session(btc_uni_universe(), scripted_wire(state));

    ASSERT_EQ(read_trusted_events(session, 2).size(), 2U);
    const VenueSessionEvent failed = session.read_event();
    ASSERT_TRUE(std::holds_alternative<TrustedBookEvent>(failed));
    const TrustedBookEvent& invalidated = std::get<TrustedBookEvent>(failed);
    EXPECT_EQ(invalidated.type, TrustedBookEventType::Invalidated);
    EXPECT_EQ(invalidated.market.product, Product("UNI", "USD"));
    EXPECT_FALSE(invalidated.book.has_value());
    EXPECT_THROW(session.read_event(), std::runtime_error);
}

TEST(KrakenUniverseSessionTest, FreshSessionIgnoresUpdateBeforeSnapshot)
{
    const std::vector<std::string> fixture = read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    );
    const auto state = wire_state({
        fixture[0],
        fixture[1],
        fixture[2],
        fixture[7],
        fixture[4]
    });
    KrakenUniverseSession session(
        MarketUniverse({Product("BTC", "USD")}),
        scripted_wire(state)
    );

    const std::vector<TrustedBookEvent> events = read_trusted_events(session, 1);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events.front().book->bids().at(100.0), 1.0);
}

TEST(KrakenUniverseRunnerTest, RediscoversAndRequiresFreshSnapshots)
{
    const std::vector<std::string> fixture = read_fixture_lines(
        "kraken_multi_product_session.jsonl"
    );
    std::vector<std::string> first_reads = first_six_fixture_frames();
    first_reads.push_back(corrupt_uni_update());
    const auto first = wire_state(first_reads);
    const auto second = wire_state({
        fixture[0],
        fixture[1],
        fixture[2],
        fixture[3],
        fixture[7],
        fixture[4]
    });
    std::size_t attempts = 0;
    bool stop = false;
    std::vector<VenueSessionEvent> observed;
    std::vector<std::chrono::milliseconds> sleeps;
    KrakenUniverseRunner runner(
        btc_uni_universe(),
        [&](const VenueSessionEvent& event)
        {
            observed.push_back(event);

            if (
                attempts == 2
                && std::holds_alternative<TrustedBookEvent>(event)
                && std::get<TrustedBookEvent>(event).type
                    == TrustedBookEventType::Snapshot
            )
            {
                stop = true;
            }
        },
        KrakenWireFactory{
            [&]() -> std::unique_ptr<KrakenWire>
            {
                ++attempts;
                return scripted_wire(attempts == 1 ? first : second);
            }
        },
        500,
        ReconnectBackoffConfig{
            std::chrono::milliseconds{2},
            std::chrono::milliseconds{4},
            0.0
        },
        [&](std::chrono::milliseconds delay)
        {
            sleeps.push_back(delay);
        }
    );

    runner.run([&stop]
    {
        return stop;
    });

    EXPECT_EQ(attempts, 2U);
    EXPECT_EQ(sleeps, (std::vector<std::chrono::milliseconds>{
        std::chrono::milliseconds{2}
    }));
    ASSERT_EQ(first->writes.size(), 2U);
    ASSERT_EQ(second->writes.size(), 2U);

    std::size_t invalidations = 0;
    std::size_t snapshots = 0;
    bool btc_disconnected = false;
    bool uni_disconnected = false;
    bool btc_reconnecting = false;
    bool uni_reconnecting = false;

    for (const VenueSessionEvent& event : observed)
    {
        if (const auto* book = std::get_if<TrustedBookEvent>(&event))
        {
            invalidations += book->type == TrustedBookEventType::Invalidated;
            snapshots += book->type == TrustedBookEventType::Snapshot;
            continue;
        }

        const auto& status = std::get<VenueMarketStatusEvent>(event);
        const bool btc = status.market.product == Product("BTC", "USD");
        btc_disconnected = btc_disconnected
            || (btc && status.status == VenueMarketStatus::Disconnected);
        uni_disconnected = uni_disconnected
            || (!btc && status.status == VenueMarketStatus::Disconnected);
        btc_reconnecting = btc_reconnecting
            || (btc && status.status == VenueMarketStatus::Reconnecting);
        uni_reconnecting = uni_reconnecting
            || (!btc && status.status == VenueMarketStatus::Reconnecting);
    }

    EXPECT_EQ(invalidations, 1U);
    EXPECT_EQ(snapshots, 3U);
    EXPECT_TRUE(btc_disconnected);
    EXPECT_TRUE(uni_disconnected);
    EXPECT_TRUE(btc_reconnecting);
    EXPECT_TRUE(uni_reconnecting);
}
