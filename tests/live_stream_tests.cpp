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

TEST(LiveStreamTest, ProductSessionClearsBacklogAndRejectsStalePublisher)
{
    LiveStreamHub hub;
    const LiveStreamSessionId sol_session =
        hub.begin_product_session("SOL-USD");
    LiveHeatmapEngine sol_engine("SOL-USD");
    hub.publish_source_status(sol_session, LiveSourceStatus::Connected);
    hub.publish(sol_session, sol_engine, sol_engine.process(snapshot()));
    const auto client = hub.subscribe();
    EXPECT_EQ(next_message(client).at("product_id").as_string(), "SOL-USD");
    EXPECT_EQ(next_message(client).at("type").as_string(), "column");
    EXPECT_EQ(next_message(client).at("status").as_string(), "live");

    const LiveHeatmapResult stale_update = sol_engine.process(message(
        "update",
        update("bid", "199.99", "9.0"),
        1,
        "2026-08-27T19:28:02.100000000Z"
    ));
    const LiveStreamSessionId eth_session =
        hub.begin_product_session("ETH-USD");
    hub.publish(sol_session, sol_engine, stale_update);
    hub.publish_source_status(
        sol_session,
        LiveSourceStatus::Disconnected,
        "stale source"
    );

    const json::object reset = next_message(client);
    EXPECT_EQ(reset.at("type").as_string(), "reset");
    EXPECT_EQ(reset.at("scope").as_string(), "product");
    EXPECT_EQ(reset.at("product_id").as_string(), "ETH-USD");
    const json::object waiting = next_message(client);
    EXPECT_EQ(waiting.at("product_id").as_string(), "ETH-USD");
    EXPECT_EQ(waiting.at("status").as_string(), "connecting");
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));

    LiveHeatmapEngine eth_engine("ETH-USD");
    const std::string eth_snapshot = message(
        "snapshot",
        update("bid", "3999.95", "1.0") + ","
            + update("offer", "4000.05", "2.0"),
        0,
        "2026-08-27T19:28:03.000000000Z",
        "ETH-USD"
    );
    hub.publish_source_status(eth_session, LiveSourceStatus::Connected);
    hub.publish(
        eth_session,
        eth_engine,
        eth_engine.process(eth_snapshot)
    );

    const json::object hello = next_message(client);
    EXPECT_EQ(hello.at("type").as_string(), "hello");
    EXPECT_EQ(hello.at("product_id").as_string(), "ETH-USD");
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(
            hello.at("config").as_object().at("price_bin_size")
        ),
        0.05
    );
    EXPECT_EQ(next_message(client).at("index").as_int64(), 0);
    EXPECT_EQ(next_message(client).at("status").as_string(), "live");
}
