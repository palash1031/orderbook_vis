#include "live_stream.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

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
    const std::string& timestamp)
{
    return
        R"({"channel":"l2_data","timestamp":")" + timestamp
        + R"(","sequence_num":)" + std::to_string(sequence)
        + R"(,"events":[{"type":")" + type
        + R"(","product_id":"SOL-USD","updates":[)" + updates
        + R"(]}]})";
}

std::string snapshot()
{
    return message(
        "snapshot",
        update("bid", "199.99", "1.0") + ","
            + update("offer", "200.01", "2.0"),
        0,
        "2026-08-27T19:28:02.000000000Z"
    );
}

json::object next_message(const std::shared_ptr<LiveStreamSubscriber>& client)
{
    const auto message = client->wait_for_message(
        std::chrono::milliseconds{0}
    );
    EXPECT_TRUE(message.has_value());
    return json::parse(*message).as_object();
}
}

TEST(LiveStreamTest, PublishesHelloStateAndColumnsToWaitingClient)
{
    LiveHeatmapEngine engine("SOL-USD");
    LiveStreamHub hub;
    const auto client = hub.subscribe();
    EXPECT_EQ(next_message(client).at("status").as_string(), "connecting");
    hub.publish_source_status(LiveSourceStatus::Connected);

    const LiveHeatmapResult result = engine.process(snapshot());
    hub.publish(engine, result);

    const json::object hello = next_message(client);
    EXPECT_EQ(hello.at("type").as_string(), "hello");
    EXPECT_EQ(hello.at("stream_mode").as_string(), "live");
    EXPECT_EQ(hello.at("product_id").as_string(), "SOL-USD");
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(
            hello.at("config").as_object().at("price_bin_size")
        ),
        0.0025
    );

    const json::object column = next_message(client);
    EXPECT_EQ(column.at("type").as_string(), "column");
    EXPECT_EQ(column.at("index").as_int64(), 0);
    EXPECT_FALSE(column.at("replace").as_bool());

    const json::object state = next_message(client);
    EXPECT_EQ(state.at("status").as_string(), "live");
    EXPECT_EQ(state.at("source_status").as_string(), "connected");
    EXPECT_EQ(state.at("book_status").as_string(), "live");
}

TEST(LiveStreamTest, PublishesOpenBucketReplacement)
{
    LiveHeatmapEngine engine("SOL-USD");
    LiveStreamHub hub;
    hub.publish_source_status(LiveSourceStatus::Connected);
    const auto client = hub.subscribe();
    hub.publish(engine, engine.process(snapshot()));
    next_message(client);
    next_message(client);
    next_message(client);

    hub.publish(engine, engine.process(message(
        "update",
        update("bid", "199.99", "3.0"),
        1,
        "2026-08-27T19:28:02.050000000Z"
    )));

    const json::object replacement = next_message(client);
    EXPECT_EQ(replacement.at("index").as_int64(), 0);
    EXPECT_TRUE(replacement.at("replace").as_bool());
}

TEST(LiveStreamTest, LateClientReceivesRollingBacklog)
{
    HeatmapConfig config;
    config.max_columns = 2;
    LiveHeatmapEngine engine("SOL-USD", config);
    LiveStreamHub hub;
    hub.publish_source_status(LiveSourceStatus::Connected);
    hub.publish(engine, engine.process(snapshot()));
    hub.publish(engine, engine.process(message(
        "update",
        update("bid", "199.99", "3.0"),
        1,
        "2026-08-27T19:28:02.100000000Z"
    )));
    hub.publish(engine, engine.process(message(
        "update",
        update("offer", "200.01", "4.0"),
        2,
        "2026-08-27T19:28:02.200000000Z"
    )));

    const auto client = hub.subscribe();
    const json::object hello = next_message(client);
    EXPECT_EQ(hello.at("first_column_index").as_int64(), 1);
    EXPECT_EQ(next_message(client).at("index").as_int64(), 1);
    EXPECT_EQ(next_message(client).at("index").as_int64(), 2);
    const json::object state = next_message(client);
    EXPECT_EQ(state.at("position").as_int64(), 3);
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(LiveStreamTest, RetainsConnectionErrorForLateClient)
{
    LiveStreamHub hub;
    hub.publish_error("Coinbase connection failed");
    const auto client = hub.subscribe();

    EXPECT_EQ(next_message(client).at("status").as_string(), "connecting");
    const json::object error = next_message(client);
    EXPECT_EQ(error.at("type").as_string(), "error");
    EXPECT_EQ(error.at("message").as_string(), "Coinbase connection failed");
}

TEST(LiveStreamTest, LateClientSeesRetainedBacklogAndDisconnection)
{
    LiveHeatmapEngine engine("SOL-USD");
    LiveStreamHub hub;
    hub.publish_source_status(LiveSourceStatus::Connected);
    hub.publish(engine, engine.process(snapshot()));
    hub.publish_source_status(
        LiveSourceStatus::Disconnected,
        "Coinbase connection closed"
    );
    hub.publish_error("Coinbase connection closed");

    const auto client = hub.subscribe();
    EXPECT_EQ(next_message(client).at("type").as_string(), "hello");
    EXPECT_EQ(next_message(client).at("type").as_string(), "column");
    const json::object state = next_message(client);
    EXPECT_EQ(state.at("status").as_string(), "disconnected");
    EXPECT_EQ(state.at("source_status").as_string(), "disconnected");
    EXPECT_EQ(state.at("book_status").as_string(), "live");
    EXPECT_EQ(
        state.at("message").as_string(),
        "Coinbase connection closed"
    );
    EXPECT_EQ(next_message(client).at("type").as_string(), "error");
}

TEST(LiveStreamTest, ReconnectClearsErrorAndWaitsForFreshSnapshot)
{
    LiveHeatmapEngine engine("SOL-USD");
    LiveStreamHub hub;
    hub.publish_source_status(LiveSourceStatus::Connected);
    hub.publish(engine, engine.process(snapshot()));
    hub.publish_source_status(LiveSourceStatus::Disconnected, "closed");
    hub.publish_error("closed");
    hub.publish(engine, engine.begin_recovery());
    hub.publish_source_status(
        LiveSourceStatus::Reconnecting,
        "closed",
        std::chrono::milliseconds{2'000}
    );
    hub.publish_source_status(LiveSourceStatus::Connected);

    const auto client = hub.subscribe();
    EXPECT_EQ(next_message(client).at("type").as_string(), "hello");
    EXPECT_EQ(next_message(client).at("type").as_string(), "column");
    const json::object state = next_message(client);
    EXPECT_EQ(state.at("status").as_string(), "waiting_for_snapshot");
    EXPECT_EQ(state.at("source_status").as_string(), "connected");
    EXPECT_EQ(state.at("book_status").as_string(), "waiting_for_snapshot");
    EXPECT_FALSE(state.contains("message"));
    EXPECT_FALSE(state.contains("retry_ms"));
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}
