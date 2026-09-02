#include "kraken_level2_stream.hpp"
#include "live_source.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
constexpr std::string_view status_message = R"({
  "channel":"status","type":"update",
  "data":[{"system":"online","version":"2.0.10"}]
})";

constexpr std::string_view heartbeat_message =
    R"({"channel":"heartbeat"})";

constexpr std::string_view instrument_ack = R"({
  "method":"subscribe","success":true,
  "result":{"channel":"instrument","snapshot":true}
})";

constexpr std::string_view online_instrument_snapshot = R"({
  "channel":"instrument","type":"snapshot","data":{
    "assets":[],
    "pairs":[
      {"symbol":"UNI/USD","base":"UNI","quote":"USD","status":"online"},
      {"symbol":"ETH/USD","base":"ETH","quote":"USD","status":"online"}
    ]
  }
})";

constexpr std::string_view missing_instrument_snapshot = R"({
  "channel":"instrument","type":"snapshot","data":{
    "assets":[],
    "pairs":[
      {"symbol":"ETH/USD","base":"ETH","quote":"USD","status":"online"}
    ]
  }
})";

constexpr std::string_view offline_instrument_snapshot = R"({
  "channel":"instrument","type":"snapshot","data":{
    "assets":[],
    "pairs":[
      {"symbol":"UNI/USD","base":"UNI","quote":"USD","status":"maintenance"}
    ]
  }
})";

constexpr std::string_view book_ack = R"({
  "method":"subscribe","success":true,
  "result":{"channel":"book","depth":500,"snapshot":true,
            "symbol":"UNI/USD"}
})";

constexpr std::string_view rejected_book_ack = R"({
  "method":"subscribe","success":false,
  "error":"Currency pair not supported UNI/USD",
  "result":{"channel":"book"}
})";

constexpr std::string_view uni_snapshot = R"({
  "channel":"book","type":"snapshot","data":[{
    "symbol":"UNI/USD",
    "bids":[{"price":4.50,"qty":3.000},{"price":4.49,"qty":4}],
    "asks":[{"price":4.51,"qty":1.000},{"price":4.52,"qty":2.000}],
    "checksum":2090218919,
    "timestamp":"2026-09-02T12:00:00.000000001Z"
  }]
})";

constexpr std::string_view corrupt_uni_update = R"({
  "channel":"book","type":"update","data":[{
    "symbol":"UNI/USD",
    "asks":[{"price":4.51,"qty":0},{"price":4.53,"qty":6.000}],
    "bids":[],
    "checksum":0,
    "timestamp":"2026-09-02T12:00:00.100000001Z"
  }]
})";

constexpr std::string_view valid_uni_update = R"({
  "channel":"book","type":"update","data":[{
    "symbol":"UNI/USD",
    "asks":[{"price":4.51,"qty":0},{"price":4.53,"qty":5.000},
            {"price":4.53,"qty":6.000}],
    "bids":[{"price":4.48,"qty":1},{"price":4.48,"qty":0}],
    "checksum":387929384,
    "timestamp":"2026-09-02T12:00:00.100000001Z"
  }]
})";

constexpr std::string_view fresh_uni_snapshot = R"({
  "channel":"book","type":"snapshot","data":[{
    "symbol":"UNI/USD",
    "bids":[{"price":4.50,"qty":3.000},{"price":4.49,"qty":4}],
    "asks":[{"price":4.51,"qty":1.000},{"price":4.52,"qty":2.000}],
    "checksum":2090218919,
    "timestamp":"2026-09-02T12:00:00.200000001Z"
  }]
})";

struct WireState
{
    std::deque<std::string> reads;
    std::vector<std::string> writes;
    std::function<void()> on_exhausted;
};

class ScriptedKrakenWire final : public KrakenWire
{
public:
    explicit ScriptedKrakenWire(std::shared_ptr<WireState> state)
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
            if (state_->on_exhausted)
            {
                state_->on_exhausted();
            }

            throw std::runtime_error("scripted Kraken wire exhausted");
        }

        std::string message = std::move(state_->reads.front());
        state_->reads.pop_front();
        return message;
    }

private:
    std::shared_ptr<WireState> state_;
};

std::shared_ptr<WireState> wire_state(
    std::initializer_list<std::string_view> messages)
{
    auto state = std::make_shared<WireState>();

    for (const std::string_view message : messages)
    {
        state->reads.emplace_back(message);
    }

    return state;
}

std::unique_ptr<KrakenWire> scripted_wire(
    const std::shared_ptr<WireState>& state)
{
    return std::make_unique<ScriptedKrakenWire>(state);
}

class RecordingKrakenSource final : public TrustedLiveMessageSource
{
public:
    RecordingKrakenSource(
        std::size_t session,
        std::unique_ptr<KrakenLevel2Stream> stream,
        std::vector<std::pair<std::size_t, TrustedBookEventType>>& observed)
        : session_(session),
          stream_(std::move(stream)),
          observed_(observed)
    {
    }

    TrustedBookEvent read_event() override
    {
        TrustedBookEvent event = stream_->read_event();
        observed_.emplace_back(session_, event.type);
        return event;
    }

private:
    std::size_t session_;
    std::unique_ptr<KrakenLevel2Stream> stream_;
    std::vector<std::pair<std::size_t, TrustedBookEventType>>& observed_;
};

std::vector<json::object> drain(
    const std::shared_ptr<LiveStreamSubscriber>& subscriber)
{
    std::vector<json::object> messages;

    while (const auto message = subscriber->wait_for_message(
        std::chrono::milliseconds{0}
    ))
    {
        messages.push_back(json::parse(*message).as_object());
    }

    return messages;
}
}

TEST(KrakenLevel2StreamTest, DiscoversAndSubscribesOnOneSession)
{
    const auto state = wire_state({
        status_message,
        instrument_ack,
        heartbeat_message,
        online_instrument_snapshot,
        book_ack,
        status_message,
        uni_snapshot
    });
    KrakenLevel2Stream stream(
        Product("UNI", "USD"),
        500,
        scripted_wire(state)
    );

    ASSERT_EQ(state->writes.size(), 2U);
    const json::object first = json::parse(state->writes[0]).as_object();
    EXPECT_EQ(
        first.at("params").as_object().at("channel").as_string(),
        "instrument"
    );
    const json::object second = json::parse(state->writes[1]).as_object();
    const json::object& params = second.at("params").as_object();
    EXPECT_EQ(params.at("channel").as_string(), "book");
    EXPECT_EQ(params.at("symbol").as_array().front().as_string(), "UNI/USD");
    EXPECT_EQ(params.at("depth").as_int64(), 500);

    const TrustedBookEvent event = stream.read_event();
    EXPECT_EQ(event.type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(event.market, (MarketKey{Venue::Kraken, Product("UNI", "USD")}));
    ASSERT_TRUE(event.book.has_value());
    EXPECT_EQ(event.book->best_bid(), 4.50);
    EXPECT_EQ(event.book->best_ask(), 4.51);
}

TEST(KrakenLevel2StreamTest, AcceptsCanonicalStringAndDefaultDepth)
{
    const auto state = wire_state({online_instrument_snapshot, uni_snapshot});
    KrakenLevel2Stream stream(" uni-usd ", scripted_wire(state));

    ASSERT_EQ(state->writes.size(), 2U);
    const json::object request = json::parse(state->writes.back()).as_object();
    EXPECT_EQ(
        request.at("params").as_object().at("depth").as_int64(),
        500
    );
    EXPECT_EQ(stream.read_event().type, TrustedBookEventType::Snapshot);
}

TEST(KrakenLevel2StreamTest, RejectsMalformedDiscoveryMessages)
{
    for (const std::string_view message : {
             std::string_view{"not json"},
             std::string_view{
                 R"({"channel":"instrument","type":"snapshot","data":{}})"
             }
         })
    {
        const auto state = wire_state({message});
        EXPECT_THROW(
            KrakenLevel2Stream(
                Product("UNI", "USD"),
                500,
                scripted_wire(state)
            ),
            std::invalid_argument
        );
    }
}

TEST(KrakenLevel2StreamTest, TreatsMissingAndOfflineMarketsAsUnsupported)
{
    for (const std::string_view message : {
             missing_instrument_snapshot,
             offline_instrument_snapshot
         })
    {
        const auto state = wire_state({message});
        EXPECT_THROW(
            KrakenLevel2Stream(
                Product("UNI", "USD"),
                500,
                scripted_wire(state)
            ),
            UnsupportedMarketError
        );
        EXPECT_EQ(state->writes.size(), 1U);
    }
}

TEST(KrakenLevel2StreamTest, SurfacesBookSubscriptionRejection)
{
    const auto state = wire_state({
        online_instrument_snapshot,
        rejected_book_ack
    });
    KrakenLevel2Stream stream(
        Product("UNI", "USD"),
        500,
        scripted_wire(state)
    );

    try
    {
        static_cast<void>(stream.read_event());
        FAIL() << "Expected the Kraken subscription rejection";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(
            std::string(error.what()).find("Currency pair not supported"),
            std::string::npos
        );
    }
}

TEST(KrakenLevel2StreamTest, RejectsInstrumentSubscriptionFailure)
{
    const auto state = wire_state({R"({
      "method":"subscribe","success":false,
      "error":"Instrument channel unavailable",
      "result":{"channel":"instrument"}
    })"});

    EXPECT_THROW(
        KrakenLevel2Stream(
            Product("UNI", "USD"),
            500,
            scripted_wire(state)
        ),
        std::runtime_error
    );
    EXPECT_EQ(state->writes.size(), 1U);
}

TEST(KrakenLevel2StreamTest, RejectsUnsupportedDepthBeforeWriting)
{
    const auto state = wire_state({online_instrument_snapshot});
    EXPECT_THROW(
        KrakenLevel2Stream(
            Product("UNI", "USD"),
            50,
            scripted_wire(state)
        ),
        std::invalid_argument
    );
    EXPECT_TRUE(state->writes.empty());
}

TEST(KrakenLevel2StreamTest, ReconnectRequiresFreshSnapshotBeforeLive)
{
    auto hub = std::make_shared<LiveStreamHub>();
    const auto subscriber = hub->subscribe();
    bool stop = false;
    std::size_t attempts = 0;
    std::vector<std::chrono::milliseconds> sleeps;
    std::vector<std::pair<std::size_t, TrustedBookEventType>> observed;
    LiveSourceRunner runner(
        "UNI-USD",
        {},
        hub,
        [&]() -> std::unique_ptr<LiveMessageSource>
        {
            const std::size_t session = attempts++;
            std::shared_ptr<WireState> state;

            if (session == 0)
            {
                state = wire_state({
                    online_instrument_snapshot,
                    uni_snapshot,
                    corrupt_uni_update
                });
            }
            else
            {
                state = wire_state({
                    online_instrument_snapshot,
                    valid_uni_update,
                    fresh_uni_snapshot
                });
                state->on_exhausted = [&stop]
                {
                    stop = true;
                };
            }

            return std::make_unique<RecordingKrakenSource>(
                session,
                std::make_unique<KrakenLevel2Stream>(
                    Product("UNI", "USD"),
                    500,
                    scripted_wire(state)
                ),
                observed
            );
        },
        ReconnectBackoffConfig{
            std::chrono::milliseconds{10},
            std::chrono::milliseconds{40},
            0.0
        },
        [&sleeps](std::chrono::milliseconds delay)
        {
            sleeps.push_back(delay);
        },
        "Kraken"
    );

    runner.run([&stop]
    {
        return stop;
    });

    EXPECT_EQ(attempts, 2U);
    EXPECT_EQ(sleeps, (std::vector<std::chrono::milliseconds>{
        std::chrono::milliseconds{10}
    }));
    EXPECT_EQ(
        observed,
        (std::vector<std::pair<std::size_t, TrustedBookEventType>>{
            {0, TrustedBookEventType::Snapshot},
            {0, TrustedBookEventType::Invalidated},
            {1, TrustedBookEventType::Snapshot}
        })
    );

    const std::vector<json::object> messages = drain(subscriber);
    std::vector<std::string> states;
    std::size_t columns = 0;

    for (const json::object& payload : messages)
    {
        const auto& type = payload.at("type").as_string();

        if (type == "state")
        {
            const auto& status = payload.at("status").as_string();
            states.emplace_back(status.c_str(), status.size());
        }
        else if (type == "column")
        {
            ++columns;
        }
    }

    const auto first_live = std::find(states.begin(), states.end(), "live");
    ASSERT_NE(first_live, states.end());
    const auto waiting = std::find(
        std::next(first_live),
        states.end(),
        "waiting_for_snapshot"
    );
    ASSERT_NE(waiting, states.end());
    const auto reconnecting = std::find(
        std::next(waiting),
        states.end(),
        "reconnecting"
    );
    ASSERT_NE(reconnecting, states.end());
    EXPECT_NE(
        std::find(std::next(reconnecting), states.end(), "live"),
        states.end()
    );
    EXPECT_EQ(columns, 2U);
}

TEST(KrakenLevel2StreamTest, UnsupportedMarketDoesNotRetry)
{
    auto hub = std::make_shared<LiveStreamHub>();
    const auto subscriber = hub->subscribe();
    std::size_t attempts = 0;
    std::size_t sleeps = 0;
    bool stop = false;
    LiveSourceRunner runner(
        "UNI-USD",
        {},
        hub,
        [&]() -> std::unique_ptr<LiveMessageSource>
        {
            ++attempts;
            throw UnsupportedMarketError(
                "Kraken does not currently support UNI-USD"
            );
        },
        ReconnectBackoffConfig{
            std::chrono::milliseconds{1},
            std::chrono::milliseconds{2},
            0.0
        },
        [&](std::chrono::milliseconds)
        {
            ++sleeps;
            stop = true;
        },
        "Kraken"
    );

    runner.run([&stop]
    {
        return stop;
    });

    EXPECT_EQ(attempts, 1U);
    EXPECT_EQ(sleeps, 0U);
    const std::vector<json::object> messages = drain(subscriber);
    bool saw_disconnected = false;
    bool saw_reconnecting = false;
    bool saw_error = false;

    for (const json::object& payload : messages)
    {
        const auto& type = payload.at("type").as_string();

        if (type == "state")
        {
            const auto& source = payload.at("source_status").as_string();
            saw_disconnected = saw_disconnected || source == "disconnected";
            saw_reconnecting = saw_reconnecting
                || source == "reconnecting";
        }
        else if (type == "error")
        {
            saw_error = payload.at("message").as_string().find(
                "does not currently support"
            ) != boost::json::string::npos;
        }
    }

    EXPECT_TRUE(saw_disconnected);
    EXPECT_FALSE(saw_reconnecting);
    EXPECT_TRUE(saw_error);
}

TEST(KrakenLiveSmokeTest, ReceivesTrustedUniSnapshot)
{
    if (std::getenv("ORDERBOOK_RUN_KRAKEN_LIVE_TESTS") == nullptr)
    {
        GTEST_SKIP() << "set ORDERBOOK_RUN_KRAKEN_LIVE_TESTS=1";
    }

    KrakenLevel2Stream stream("UNI-USD", 10);
    const TrustedBookEvent event = stream.read_event();

    EXPECT_EQ(event.type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(event.market, (MarketKey{Venue::Kraken, Product("UNI", "USD")}));
    ASSERT_TRUE(event.book.has_value());
    EXPECT_TRUE(event.book->best_bid().has_value());
    EXPECT_TRUE(event.book->best_ask().has_value());
}
