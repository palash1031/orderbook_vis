#include "coinbase_level2_stream.hpp"
#include "coinbase_universe_session.hpp"
#include "coinbase_wire.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
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

class ScriptedCoinbaseWire final : public CoinbaseWire
{
public:
    explicit ScriptedCoinbaseWire(std::shared_ptr<WireState> state)
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
            throw std::runtime_error("scripted Coinbase wire exhausted");
        }

        std::string message = std::move(state_->reads.front());
        state_->reads.pop_front();
        return message;
    }

private:
    std::shared_ptr<WireState> state_;
};

std::unique_ptr<CoinbaseWire> scripted_wire(
    const std::shared_ptr<WireState>& state)
{
    return std::make_unique<ScriptedCoinbaseWire>(state);
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

std::string update(
    std::string_view side,
    std::string_view price,
    std::string_view quantity)
{
    return R"({"side":")" + std::string(side)
        + R"(","price_level":")" + std::string(price)
        + R"(","new_quantity":")" + std::string(quantity) + R"("})";
}

std::string book_message(
    std::string_view type,
    std::string_view product,
    std::uint64_t sequence,
    std::string_view updates)
{
    return R"({"channel":"l2_data","timestamp":"2026-09-08T12:00:00.000000001Z","sequence_num":)"
        + std::to_string(sequence)
        + R"(,"events":[{"type":")" + std::string(type)
        + R"(","product_id":")" + std::string(product)
        + R"(","updates":[)" + std::string(updates) + R"(]}]})";
}

std::string heartbeat(std::uint64_t sequence)
{
    return R"({"channel":"heartbeats","timestamp":"2026-09-08T12:00:00.000000001Z","sequence_num":)"
        + std::to_string(sequence)
        + R"(,"events":[{"heartbeat_counter":"1"}]})";
}

std::shared_ptr<WireState> wire_state(
    const std::vector<std::string>& reads)
{
    auto state = std::make_shared<WireState>();
    state->reads.assign(reads.begin(), reads.end());
    return state;
}

MarketUniverse btc_uni_universe()
{
    return MarketUniverse({Product("BTC", "USD"), Product("UNI", "USD")});
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

std::vector<VenueMarketStatusEvent> read_status_events(
    VenueUniverseSession& session,
    std::size_t count)
{
    std::vector<VenueMarketStatusEvent> statuses;

    while (statuses.size() < count)
    {
        VenueSessionEvent event = session.read_event();

        if (const auto* status = std::get_if<VenueMarketStatusEvent>(&event))
        {
            statuses.push_back(*status);
        }
    }

    return statuses;
}

class ScriptedVenueSession final : public VenueUniverseSession
{
public:
    explicit ScriptedVenueSession(std::deque<VenueSessionEvent> events)
        : events_(std::move(events))
    {
    }

    Venue venue() const noexcept override
    {
        return Venue::Coinbase;
    }

    VenueSessionEvent read_event() override
    {
        if (events_.empty())
        {
            throw std::runtime_error("scripted Coinbase session closed");
        }

        VenueSessionEvent event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

private:
    std::deque<VenueSessionEvent> events_;
};

TrustedBookEvent snapshot_event(const Product& product, double bid)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, bid, 1.0);
    book.apply_update(BookSide::Offer, bid + 1.0, 2.0);
    return {
        TrustedBookEventType::Snapshot,
        {Venue::Coinbase, product},
        MarketTimestamp{std::chrono::nanoseconds{1}},
        std::move(book)
    };
}
}

TEST(CoinbaseLevel2StreamTest, PreservesSingleProductWireBehavior)
{
    const auto state = wire_state({"unchanged raw frame"});
    CoinbaseLevel2Stream stream(" sol-usd ", scripted_wire(state));

    ASSERT_EQ(state->writes.size(), 2U);
    const json::object level2 = json::parse(state->writes[0]).as_object();
    EXPECT_EQ(level2.at("channel").as_string(), "level2");
    ASSERT_EQ(level2.at("product_ids").as_array().size(), 1U);
    EXPECT_EQ(
        level2.at("product_ids").as_array().front().as_string(),
        "SOL-USD"
    );
    const json::object heartbeat_message =
        json::parse(state->writes[1]).as_object();
    EXPECT_EQ(heartbeat_message.at("channel").as_string(), "heartbeats");
    EXPECT_EQ(stream.read(), "unchanged raw frame");
}

TEST(CoinbaseUniverseSessionTest, BatchesDefaultUniverseSubscription)
{
    const MarketUniverse universe = MarketUniverse::default_usd();
    const auto state = wire_state({});
    CoinbaseUniverseSession session(universe, scripted_wire(state));

    ASSERT_EQ(state->writes.size(), 2U);

    const json::object heartbeat_message =
        json::parse(state->writes[0]).as_object();
    EXPECT_EQ(heartbeat_message.at("channel").as_string(), "heartbeats");

    const json::object level2_message =
        json::parse(state->writes[1]).as_object();
    EXPECT_EQ(level2_message.at("channel").as_string(), "level2");

    const json::array& products =
        level2_message.at("product_ids").as_array();
    ASSERT_EQ(products.size(), universe.size());

    for (std::size_t index = 0; index < products.size(); ++index)
    {
        EXPECT_EQ(
            products[index].as_string(),
            universe.products()[index].to_string()
        );
    }
}

TEST(CoinbaseUniverseSessionTest, ReconstructsInterleavedProductsOnOneWire)
{
    const auto state = wire_state(read_fixture_lines(
        "coinbase_btc_uni_sequence.jsonl"
    ));
    CoinbaseUniverseSession session(btc_uni_universe(), scripted_wire(state));

    ASSERT_EQ(state->writes.size(), 2U);
    EXPECT_EQ(
        json::parse(state->writes[0]).as_object().at("channel").as_string(),
        "heartbeats"
    );

    const json::object request = json::parse(state->writes[1]).as_object();
    EXPECT_EQ(request.at("channel").as_string(), "level2");
    const json::array& products = request.at("product_ids").as_array();
    ASSERT_EQ(products.size(), 2U);
    EXPECT_EQ(products[0].as_string(), "BTC-USD");
    EXPECT_EQ(products[1].as_string(), "UNI-USD");

    const std::vector<TrustedBookEvent> events = read_trusted_events(session, 4);
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events[0].market.product, Product("BTC", "USD"));
    EXPECT_EQ(events[1].type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events[1].market.product, Product("UNI", "USD"));
    EXPECT_EQ(events[2].type, TrustedBookEventType::Update);
    ASSERT_TRUE(events[2].book.has_value());
    EXPECT_EQ(events[2].book->best_bid(), 80002.0);
    EXPECT_EQ(events[2].book->best_ask(), 80001.0);
    EXPECT_EQ(events[3].type, TrustedBookEventType::Update);
    ASSERT_TRUE(events[3].book.has_value());
    EXPECT_EQ(events[3].book->best_bid(), 4.57);
    EXPECT_EQ(events[3].book->best_ask(), 4.59);
}

TEST(CoinbaseUniverseSessionTest, GlobalGapInvalidatesEveryConfiguredBook)
{
    const auto state = wire_state({
        book_message(
            "snapshot",
            "BTC-USD",
            0,
            update("bid", "100", "1") + "," + update("offer", "101", "1")
        ),
        book_message(
            "snapshot",
            "UNI-USD",
            1,
            update("bid", "4.5", "1") + "," + update("offer", "4.6", "1")
        ),
        heartbeat(3),
        book_message("update", "BTC-USD", 4, update("bid", "102", "9"))
    });
    CoinbaseUniverseSession session(btc_uni_universe(), scripted_wire(state));
    ASSERT_EQ(read_trusted_events(session, 2).size(), 2U);

    const VenueSessionEvent first = session.read_event();
    const VenueSessionEvent second = session.read_event();
    ASSERT_TRUE(std::holds_alternative<TrustedBookEvent>(first));
    ASSERT_TRUE(std::holds_alternative<TrustedBookEvent>(second));
    const auto& first_book = std::get<TrustedBookEvent>(first);
    const auto& second_book = std::get<TrustedBookEvent>(second);
    EXPECT_EQ(first_book.type, TrustedBookEventType::Invalidated);
    EXPECT_EQ(second_book.type, TrustedBookEventType::Invalidated);
    EXPECT_EQ(first_book.market.product, Product("BTC", "USD"));
    EXPECT_EQ(second_book.market.product, Product("UNI", "USD"));
    EXPECT_THROW(session.read_event(), std::runtime_error);
}

TEST(CoinbaseUniverseSessionTest, RecoveryAllowsEachSnapshotIndependently)
{
    const auto state = wire_state({
        book_message("update", "UNI-USD", 0, update("bid", "9", "1")),
        book_message(
            "snapshot",
            "BTC-USD",
            1,
            update("bid", "110", "1") + "," + update("offer", "111", "1")
        ),
        book_message("update", "UNI-USD", 2, update("bid", "10", "1"))
    });
    CoinbaseUniverseSession session(btc_uni_universe(), scripted_wire(state));

    const std::vector<TrustedBookEvent> events = read_trusted_events(session, 1);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(events.front().market.product, Product("BTC", "USD"));
    EXPECT_EQ(events.front().book->best_bid(), 110.0);
}

TEST(CoinbaseUniverseSessionTest, AttributablePermanentRejectionIsUnsupported)
{
    const auto state = wire_state({
        R"({"type":"error","product_id":"UNI-USD","message":"product not supported"})",
        book_message(
            "snapshot",
            "BTC-USD",
            0,
            update("bid", "100", "1") + "," + update("offer", "101", "1")
        )
    });
    CoinbaseUniverseSession session(btc_uni_universe(), scripted_wire(state));

    const std::vector<VenueMarketStatusEvent> statuses =
        read_status_events(session, 5);
    ASSERT_EQ(statuses.size(), 5U);
    EXPECT_EQ(statuses.back().market.product, Product("UNI", "USD"));
    EXPECT_EQ(statuses.back().status, VenueMarketStatus::Unsupported);

    const std::vector<TrustedBookEvent> trusted = read_trusted_events(session, 1);
    ASSERT_EQ(trusted.size(), 1U);
    EXPECT_EQ(trusted.front().market.product, Product("BTC", "USD"));
}

TEST(CoinbaseUniverseSessionTest, GenericErrorIsSessionFailureNotUnsupported)
{
    const auto state = wire_state({
        R"({"type":"error","message":"temporary service failure"})"
    });
    CoinbaseUniverseSession session(btc_uni_universe(), scripted_wire(state));

    read_status_events(session, 4);
    EXPECT_THROW(session.read_event(), std::runtime_error);
}

TEST(CoinbaseUniverseRunnerTest, ReconnectsWithoutMakingFreshUpdatesLive)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    std::size_t attempts = 0;
    bool stop = false;
    std::vector<VenueSessionEvent> observed;
    std::vector<std::chrono::milliseconds> sleeps;
    CoinbaseUniverseRunner runner(
        btc_uni_universe(),
        [&](const VenueSessionEvent& event)
        {
            observed.push_back(event);

            if (
                attempts == 2
                && std::holds_alternative<TrustedBookEvent>(event)
            )
            {
                stop = true;
            }
        },
        [&]() -> std::unique_ptr<VenueUniverseSession>
        {
            ++attempts;

            if (attempts == 1)
            {
                return std::make_unique<ScriptedVenueSession>(
                    std::deque<VenueSessionEvent>{snapshot_event(btc, 100.0)}
                );
            }

            return std::make_unique<ScriptedVenueSession>(
                std::deque<VenueSessionEvent>{
                    VenueMarketStatusEvent{
                        {Venue::Coinbase, btc},
                        VenueMarketStatus::WaitingForSnapshot
                    },
                    VenueMarketStatusEvent{
                        {Venue::Coinbase, uni},
                        VenueMarketStatus::WaitingForSnapshot
                    },
                    snapshot_event(btc, 101.0)
                }
            );
        },
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

    bool btc_disconnected = false;
    bool uni_disconnected = false;
    bool btc_reconnecting = false;
    bool uni_reconnecting = false;

    for (const VenueSessionEvent& event : observed)
    {
        const auto* status = std::get_if<VenueMarketStatusEvent>(&event);

        if (!status)
        {
            continue;
        }

        const bool is_btc = status->market.product == btc;
        btc_disconnected = btc_disconnected
            || (is_btc && status->status == VenueMarketStatus::Disconnected);
        uni_disconnected = uni_disconnected
            || (!is_btc && status->status == VenueMarketStatus::Disconnected);
        btc_reconnecting = btc_reconnecting
            || (is_btc && status->status == VenueMarketStatus::Reconnecting);
        uni_reconnecting = uni_reconnecting
            || (!is_btc && status->status == VenueMarketStatus::Reconnecting);
    }

    EXPECT_TRUE(btc_disconnected);
    EXPECT_TRUE(uni_disconnected);
    EXPECT_TRUE(btc_reconnecting);
    EXPECT_TRUE(uni_reconnecting);
}

TEST(CoinbaseUniverseRunnerTest, OmitsPermanentlyRejectedProductOnRetry)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const auto first = wire_state({
        R"({"type":"error","product_id":"UNI-USD","message":"unknown product"})"
    });
    const auto second = wire_state({
        book_message(
            "snapshot",
            "BTC-USD",
            0,
            update("bid", "100", "1") + "," + update("offer", "101", "1")
        )
    });
    std::size_t attempts = 0;
    bool stop = false;
    std::vector<VenueSessionEvent> observed;
    CoinbaseUniverseRunner runner(
        btc_uni_universe(),
        [&](const VenueSessionEvent& event)
        {
            observed.push_back(event);

            if (
                attempts == 2
                && std::holds_alternative<TrustedBookEvent>(event)
            )
            {
                stop = true;
            }
        },
        CoinbaseWireFactory{
            [&]() -> std::unique_ptr<CoinbaseWire>
            {
                ++attempts;
                return scripted_wire(attempts == 1 ? first : second);
            }
        },
        ReconnectBackoffConfig{
            std::chrono::milliseconds{1},
            std::chrono::milliseconds{1},
            0.0
        },
        [](std::chrono::milliseconds)
        {
        }
    );

    runner.run([&stop]
    {
        return stop;
    });

    ASSERT_EQ(attempts, 2U);
    ASSERT_EQ(second->writes.size(), 2U);
    EXPECT_EQ(
        json::parse(second->writes[0]).as_object().at("channel").as_string(),
        "heartbeats"
    );
    const json::object retry_request =
        json::parse(second->writes[1]).as_object();
    const json::array& retry_products =
        retry_request.at("product_ids").as_array();
    ASSERT_EQ(retry_products.size(), 1U);
    EXPECT_EQ(retry_products.front().as_string(), "BTC-USD");

    const bool unsupported_preserved = std::any_of(
        observed.begin(),
        observed.end(),
        [&](const VenueSessionEvent& event)
        {
            const auto* status = std::get_if<VenueMarketStatusEvent>(&event);
            return status
                && status->market.product == uni
                && status->status == VenueMarketStatus::Unsupported;
        }
    );
    EXPECT_TRUE(unsupported_preserved);
}

TEST(CoinbaseUniverseLiveSmokeTest, CharacterizesSessionSequence)
{
    const char* enabled = std::getenv("ORDERBOOK_RUN_COINBASE_LIVE_TESTS");

    if (enabled == nullptr || std::string_view(enabled) != "1")
    {
        GTEST_SKIP() << "set ORDERBOOK_RUN_COINBASE_LIVE_TESTS=1";
    }

    CoinbaseUniverseSession session(
        btc_uni_universe(),
        make_coinbase_wire()
    );
    bool btc_live = false;
    bool uni_live = false;

    for (std::size_t reads = 0; reads < 200 && !(btc_live && uni_live); ++reads)
    {
        const VenueSessionEvent event = session.read_event();
        const auto* trusted = std::get_if<TrustedBookEvent>(&event);

        if (!trusted || trusted->type != TrustedBookEventType::Snapshot)
        {
            continue;
        }

        btc_live = btc_live || trusted->market.product == Product("BTC", "USD");
        uni_live = uni_live || trusted->market.product == Product("UNI", "USD");
    }

    EXPECT_TRUE(btc_live);
    EXPECT_TRUE(uni_live);
}
