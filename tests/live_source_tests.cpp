#include "live_source.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
std::string update(
    const std::string& side,
    const std::string& price,
    const std::string& quantity)
{
    return
        R"({"side":")" + side
        + R"(","event_time":"...","price_level":")" + price
        + R"(","new_quantity":")" + quantity
        + R"("})";
}

std::string message(
    const std::string& type,
    const std::string& updates,
    unsigned int sequence,
    const std::string& timestamp,
    const std::string& product_id = "SOL-USD")
{
    return
        R"({"channel":"l2_data","timestamp":")" + timestamp
        + R"(","sequence_num":)" + std::to_string(sequence)
        + R"(,"events":[{"type":")" + type
        + R"(","product_id":")" + product_id + R"(","updates":[)" + updates
        + R"(]}]})";
}

std::string snapshot(unsigned int sequence, const std::string& timestamp)
{
    return message(
        "snapshot",
        update("bid", "199.99", "1.0") + ","
            + update("offer", "200.01", "2.0"),
        sequence,
        timestamp
    );
}

class ScriptedSource final : public LiveMessageSource
{
public:
    ScriptedSource(
        std::vector<std::string> messages,
        std::function<void()> on_exhausted = {})
        : messages_(std::move(messages)),
          on_exhausted_(std::move(on_exhausted))
    {
    }

    std::string read() override
    {
        if (position_ < messages_.size())
        {
            return messages_[position_++];
        }

        if (on_exhausted_)
        {
            on_exhausted_();
        }

        throw std::runtime_error("scripted connection closed");
    }

private:
    std::vector<std::string> messages_;
    std::function<void()> on_exhausted_;
    std::size_t position_ = 0;
};

class ScriptedTrustedSource final : public TrustedLiveMessageSource
{
public:
    ScriptedTrustedSource(
        std::vector<TrustedBookEvent> events,
        std::function<void()> on_exhausted)
        : events_(std::move(events)),
          on_exhausted_(std::move(on_exhausted))
    {
    }

    TrustedBookEvent read_event() override
    {
        if (position_ < events_.size())
        {
            return events_[position_++];
        }

        on_exhausted_();
        throw std::runtime_error("trusted script complete");
    }

private:
    std::vector<TrustedBookEvent> events_;
    std::function<void()> on_exhausted_;
    std::size_t position_ = 0;
};

TrustedBookEvent trusted_uni_snapshot()
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 4.50, 1.0);
    book.apply_update(BookSide::Offer, 4.52, 2.0);
    return {
        TrustedBookEventType::Snapshot,
        {Venue::Kraken, Product("UNI", "USD")},
        MarketTimestamp{std::chrono::nanoseconds{1}},
        std::move(book)
    };
}

std::vector<json::object> drain(
    const std::shared_ptr<LiveStreamSubscriber>& client)
{
    std::vector<json::object> messages;

    while (const auto message = client->wait_for_message(
        std::chrono::milliseconds{0}
    ))
    {
        messages.push_back(json::parse(*message).as_object());
    }

    return messages;
}
}

TEST(ReconnectBackoffTest, DoublesCapsAndResets)
{
    const ReconnectBackoffConfig config{
        std::chrono::milliseconds{100},
        std::chrono::milliseconds{400},
        0.0
    };
    ReconnectBackoff backoff(config, 7);

    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds{100});
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds{200});
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds{400});
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds{400});
    backoff.reset();
    EXPECT_EQ(backoff.next_delay(), std::chrono::milliseconds{100});
}

TEST(ReconnectBackoffTest, JitterStaysWithinCappedRanges)
{
    const ReconnectBackoffConfig config{
        std::chrono::milliseconds{1'000},
        std::chrono::milliseconds{4'000},
        0.2
    };
    ReconnectBackoff backoff(config, 11);

    const auto first = backoff.next_delay();
    const auto second = backoff.next_delay();
    const auto capped = backoff.next_delay();
    EXPECT_GE(first, std::chrono::milliseconds{800});
    EXPECT_LE(first, std::chrono::milliseconds{1'000});
    EXPECT_GE(second, std::chrono::milliseconds{1'600});
    EXPECT_LE(second, std::chrono::milliseconds{2'000});
    EXPECT_GE(capped, std::chrono::milliseconds{3'200});
    EXPECT_LE(capped, std::chrono::milliseconds{4'000});
}

TEST(ReconnectBackoffTest, RejectsInvalidConfiguration)
{
    EXPECT_THROW(
        ReconnectBackoff({
            std::chrono::milliseconds{0},
            std::chrono::milliseconds{100},
            0.0
        }),
        std::invalid_argument
    );
    EXPECT_THROW(
        ReconnectBackoff({
            std::chrono::milliseconds{200},
            std::chrono::milliseconds{100},
            0.0
        }),
        std::invalid_argument
    );
    EXPECT_THROW(
        ReconnectBackoff({
            std::chrono::milliseconds{100},
            std::chrono::milliseconds{200},
            1.0
        }),
        std::invalid_argument
    );
}

TEST(LiveSourceRunnerTest, ReconnectsAndRequiresFreshSnapshot)
{
    auto hub = std::make_shared<LiveStreamHub>();
    const auto client = hub->subscribe();
    bool stop = false;
    std::size_t attempts = 0;
    std::vector<std::chrono::milliseconds> sleeps;
    LiveSourceRunner runner(
        "SOL-USD",
        {},
        hub,
        [&]() -> std::unique_ptr<LiveMessageSource>
        {
            ++attempts;

            if (attempts == 1)
            {
                return std::make_unique<ScriptedSource>(
                    std::vector<std::string>{snapshot(
                        0,
                        "2026-08-27T19:28:02.000000000Z"
                    )}
                );
            }

            return std::make_unique<ScriptedSource>(
                std::vector<std::string>{
                    message(
                        "update",
                        update("bid", "199.99", "9.0"),
                        0,
                        "2026-08-27T19:28:02.200000000Z"
                    ),
                    snapshot(1, "2026-08-27T19:28:02.500000000Z")
                },
                [&stop]
                {
                    stop = true;
                }
            );
        },
        {
            std::chrono::milliseconds{10},
            std::chrono::milliseconds{40},
            0.0
        },
        [&sleeps](std::chrono::milliseconds delay)
        {
            sleeps.push_back(delay);
        }
    );

    runner.run([&stop]
    {
        return stop;
    });

    EXPECT_EQ(attempts, 2U);
    ASSERT_EQ(sleeps.size(), 1U);
    EXPECT_EQ(sleeps.front(), std::chrono::milliseconds{10});

    const std::vector<json::object> messages = drain(client);
    std::vector<std::int64_t> column_indices;
    bool saw_reconnecting = false;
    bool saw_waiting_for_snapshot = false;

    for (const json::object& payload : messages)
    {
        const auto& type = payload.at("type").as_string();

        if (type == "column")
        {
            column_indices.push_back(payload.at("index").as_int64());
        }
        else if (type == "state")
        {
            const auto& status = payload.at("status").as_string();
            saw_reconnecting = saw_reconnecting || status == "reconnecting";
            saw_waiting_for_snapshot = saw_waiting_for_snapshot
                || status == "waiting_for_snapshot";
        }
    }

    EXPECT_EQ(column_indices, (std::vector<std::int64_t>{0, 1}));
    EXPECT_TRUE(saw_reconnecting);
    EXPECT_TRUE(saw_waiting_for_snapshot);
}

TEST(LiveSourceRunnerTest, SequenceGapForcesReconnect)
{
    auto hub = std::make_shared<LiveStreamHub>();
    const auto client = hub->subscribe();
    bool stop = false;
    std::size_t attempts = 0;
    LiveSourceRunner runner(
        "SOL-USD",
        {},
        hub,
        [&]() -> std::unique_ptr<LiveMessageSource>
        {
            ++attempts;

            if (attempts == 1)
            {
                return std::make_unique<ScriptedSource>(
                    std::vector<std::string>{
                        snapshot(0, "2026-08-27T19:28:02.000000000Z"),
                        message(
                            "update",
                            update("bid", "199.99", "3.0"),
                            2,
                            "2026-08-27T19:28:02.200000000Z"
                        )
                    }
                );
            }

            return std::make_unique<ScriptedSource>(
                std::vector<std::string>{snapshot(
                    0,
                    "2026-08-27T19:28:02.500000000Z"
                )},
                [&stop]
                {
                    stop = true;
                }
            );
        },
        {
            std::chrono::milliseconds{1},
            std::chrono::milliseconds{2},
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

    EXPECT_EQ(attempts, 2U);
    const std::vector<json::object> messages = drain(client);
    bool saw_gap = false;
    bool saw_reconnecting = false;

    for (const json::object& payload : messages)
    {
        if (payload.at("type").as_string() != "state")
        {
            continue;
        }

        const auto& status = payload.at("status").as_string();
        saw_gap = saw_gap || status == "gap";
        saw_reconnecting = saw_reconnecting || status == "reconnecting";
    }

    EXPECT_TRUE(saw_gap);
    EXPECT_TRUE(saw_reconnecting);
}

TEST(LiveSourceRunnerTest, PublishesTrustedVenueEventsWithoutCoinbaseParsing)
{
    auto hub = std::make_shared<LiveStreamHub>();
    const auto client = hub->subscribe();
    bool stop = false;
    LiveSourceRunner runner(
        "UNI-USD",
        {},
        hub,
        [&]() -> std::unique_ptr<LiveMessageSource>
        {
            return std::make_unique<ScriptedTrustedSource>(
                std::vector<TrustedBookEvent>{trusted_uni_snapshot()},
                [&stop]
                {
                    stop = true;
                }
            );
        },
        ReconnectBackoffConfig{},
        [](std::chrono::milliseconds)
        {
        },
        "Kraken"
    );

    runner.run([&stop]
    {
        return stop;
    });

    const std::vector<json::object> messages = drain(client);
    const bool published_column = std::any_of(
        messages.begin(),
        messages.end(),
        [](const json::object& payload)
        {
            return payload.at("type").as_string() == "column";
        }
    );
    EXPECT_TRUE(published_column);
}

TEST(LiveMarketServiceTest, NormalizesAndValidatesSwitchControls)
{
    auto hub = std::make_shared<LiveStreamHub>();
    LiveMarketService service(
        "sol-usd",
        {},
        hub,
        [](std::string_view) -> std::unique_ptr<LiveMessageSource>
        {
            return nullptr;
        }
    );
    const auto client = hub->subscribe();
    const std::vector<json::object> initial = drain(client);
    ASSERT_EQ(initial.size(), 1U);
    EXPECT_EQ(initial.front().at("product_id").as_string(), "SOL-USD");

    service.apply_control(
        R"({"action":"switch_product","product_id":" eth-usd "})"
    );
    EXPECT_EQ(service.product_id(), "ETH-USD");
    const std::vector<json::object> switched = drain(client);
    ASSERT_EQ(switched.size(), 2U);
    EXPECT_EQ(switched[0].at("type").as_string(), "reset");
    EXPECT_EQ(switched[0].at("product_id").as_string(), "ETH-USD");
    EXPECT_EQ(switched[1].at("status").as_string(), "connecting");

    EXPECT_THROW(
        service.apply_control(
            R"({"action":"switch_product","product_id":"ETH/USD"})"
        ),
        std::invalid_argument
    );
    EXPECT_THROW(
        service.apply_control(R"({"action":"pause"})"),
        std::invalid_argument
    );
    EXPECT_THROW(service.apply_control("not json"), std::invalid_argument);
}

TEST(LiveMarketServiceTest, RunningServiceSwitchesToFreshProductSnapshot)
{
    auto hub = std::make_shared<LiveStreamHub>();
    LiveMarketService* service_pointer = nullptr;
    bool stop = false;
    std::vector<std::string> opened_products;
    LiveMarketService service(
        "SOL-USD",
        {},
        hub,
        [&](std::string_view product_id)
            -> std::unique_ptr<LiveMessageSource>
        {
            opened_products.emplace_back(product_id);

            if (product_id == "SOL-USD")
            {
                return std::make_unique<ScriptedSource>(
                    std::vector<std::string>{snapshot(
                        0,
                        "2026-08-27T19:28:02.000000000Z"
                    )},
                    [&]
                    {
                        service_pointer->switch_product("ETH-USD");
                    }
                );
            }

            return std::make_unique<ScriptedSource>(
                std::vector<std::string>{message(
                    "snapshot",
                    update("bid", "3999.95", "1.0") + ","
                        + update("offer", "4000.05", "2.0"),
                    0,
                    "2026-08-27T19:28:03.000000000Z",
                    "ETH-USD"
                )},
                [&stop]
                {
                    stop = true;
                }
            );
        },
        {
            std::chrono::milliseconds{1},
            std::chrono::milliseconds{2},
            0.0
        },
        [](std::chrono::milliseconds)
        {
        }
    );
    service_pointer = &service;
    const auto client = hub->subscribe();

    service.run([&stop]
    {
        return stop;
    });

    EXPECT_EQ(
        opened_products,
        (std::vector<std::string>{"SOL-USD", "ETH-USD"})
    );
    EXPECT_EQ(service.product_id(), "ETH-USD");
    const std::vector<json::object> messages = drain(client);
    ASSERT_GE(messages.size(), 3U);
    EXPECT_EQ(messages[0].at("type").as_string(), "hello");
    EXPECT_EQ(messages[0].at("product_id").as_string(), "ETH-USD");
    EXPECT_EQ(messages[1].at("type").as_string(), "column");
    EXPECT_EQ(messages[1].at("index").as_int64(), 0);
    EXPECT_EQ(messages[2].at("status").as_string(), "live");
}
