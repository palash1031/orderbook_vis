#include "replay_stream.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace json = boost::json;

namespace
{
const std::string replay_document = R"({
    "schema_version": 1,
    "product_id": "BTC-USD",
    "config": {
        "time_bucket_ns": 100000000,
        "price_bin_size": 1.0,
        "price_bin_count": 3,
        "max_columns": 10
    },
    "columns": [
        {
            "timestamp_ns": "1000000000",
            "timestamp_ms": 1000,
            "first_price": 99.0,
            "mid_price": 100.0,
            "bids": [1.0, 0.0, 0.0],
            "asks": [0.0, 0.0, 2.0]
        },
        {
            "timestamp_ns": "1100000000",
            "timestamp_ms": 1100,
            "first_price": 99.0,
            "mid_price": 100.5,
            "bids": [1.5, 0.0, 0.0],
            "asks": [0.0, 0.0, 2.5]
        },
        {
            "timestamp_ns": "1300000000",
            "timestamp_ms": 1300,
            "first_price": 100.0,
            "mid_price": 101.0,
            "bids": [3.0, 0.0, 0.0],
            "asks": [0.0, 0.0, 4.0]
        }
    ]
})";
}

TEST(ReplayStreamTest, ParsesMetadataAndColumnPayloads)
{
    const ReplayStreamData replay = parse_replay_stream(replay_document);

    EXPECT_EQ(replay.product_id, "BTC-USD");
    ASSERT_EQ(replay.columns.size(), 3U);
    EXPECT_EQ(replay.columns[0].timestamp_ms, 1000);
    EXPECT_EQ(replay.columns[2].timestamp_ms, 1300);

    const json::object hello =
        json::parse(replay.hello_message_json).as_object();
    EXPECT_EQ(hello.at("type").as_string(), "hello");
    EXPECT_EQ(hello.at("total_columns").as_int64(), 3);
    EXPECT_EQ(hello.at("start_timestamp_ms").as_int64(), 1000);
    EXPECT_EQ(hello.at("end_timestamp_ms").as_int64(), 1300);
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(hello.at("peak_depth")),
        4.0
    );

    const json::object last_column =
        json::parse(replay.columns.back().payload_json).as_object();
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(last_column.at("mid_price")),
        101.0
    );
}

TEST(ReplayStreamTest, PlaysPausesChangesSpeedAndCompletes)
{
    const ReplayStreamData replay = parse_replay_stream(replay_document);
    ReplayPlayback playback(replay);

    EXPECT_EQ(playback.status(), ReplayStatus::Paused);
    EXPECT_FALSE(playback.take_next_column_message().has_value());

    playback.apply_control(R"({"action":"set_speed","speed":5})");
    playback.apply_control(R"({"action":"play"})");
    const auto first = playback.take_next_column_message();

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(playback.position(), 1U);
    EXPECT_EQ(playback.speed(), 5U);
    EXPECT_EQ(playback.interval_to_next(), std::chrono::milliseconds{20});
    EXPECT_EQ(json::parse(*first).as_object().at("index").as_int64(), 0);

    playback.apply_control(R"({"action":"pause"})");
    EXPECT_EQ(playback.status(), ReplayStatus::Paused);
    EXPECT_FALSE(playback.take_next_column_message().has_value());

    playback.apply_control(R"({"action":"play"})");
    ASSERT_TRUE(playback.take_next_column_message().has_value());
    EXPECT_EQ(playback.interval_to_next(), std::chrono::milliseconds{40});

    playback.apply_control(R"({"action":"set_speed","speed":20})");
    EXPECT_EQ(playback.interval_to_next(), std::chrono::milliseconds{10});
    ASSERT_TRUE(playback.take_next_column_message().has_value());
    EXPECT_EQ(playback.status(), ReplayStatus::Complete);

    const json::object complete =
        json::parse(playback.state_message()).as_object();
    EXPECT_EQ(complete.at("status").as_string(), "complete");
    EXPECT_EQ(complete.at("position").as_int64(), 3);
}

TEST(ReplayStreamTest, RestartReturnsToPausedBeginning)
{
    const ReplayStreamData replay = parse_replay_stream(replay_document);
    ReplayPlayback playback(replay);
    playback.apply_control(R"({"action":"play"})");
    playback.take_next_column_message();

    const ReplayControlResult result = playback.apply_control(
        R"({"action":"restart"})"
    );

    EXPECT_EQ(result, ReplayControlResult::Restarted);
    EXPECT_EQ(playback.status(), ReplayStatus::Paused);
    EXPECT_EQ(playback.position(), 0U);
}

TEST(ReplayStreamTest, RejectsInvalidControls)
{
    const ReplayStreamData replay = parse_replay_stream(replay_document);
    ReplayPlayback playback(replay);

    EXPECT_THROW(
        playback.apply_control(R"({"action":"set_speed","speed":2})"),
        std::invalid_argument
    );
    EXPECT_THROW(
        playback.apply_control(R"({"action":"seek"})"),
        std::invalid_argument
    );
    EXPECT_THROW(playback.apply_control("not json"), std::invalid_argument);
}

TEST(ReplayStreamTest, RejectsInvalidColumnDimensions)
{
    const std::string invalid = R"({
        "schema_version":1,
        "product_id":"BTC-USD",
        "config":{"price_bin_count":3},
        "columns":[{
            "timestamp_ms":1000,
            "first_price":99.0,
            "mid_price":100.0,
            "bids":[1.0],
            "asks":[2.0]
        }]
    })";

    EXPECT_THROW(parse_replay_stream(invalid), std::invalid_argument);
}
