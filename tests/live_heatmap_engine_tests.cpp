#include "live_heatmap_engine.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

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
    const std::string& product = "ETH-USD")
{
    return
        R"({"channel":"l2_data","timestamp":")" + timestamp
        + R"(","sequence_num":)" + std::to_string(sequence)
        + R"(,"events":[{"type":")" + type
        + R"(","product_id":")" + product
        + R"(","updates":[)" + updates
        + R"(]}]})";
}

std::string snapshot(
    unsigned int sequence = 0,
    const std::string& timestamp = "2026-08-27T19:28:02.000000000Z")
{
    return message(
        "snapshot",
        update("bid", "3999.95", "1.5") + ","
            + update("offer", "4000.05", "2.5"),
        sequence,
        timestamp
    );
}
}

TEST(LiveHeatmapEngineTest, BuildsFirstColumnFromSnapshot)
{
    LiveHeatmapEngine engine(" eth-usd ");
    const LiveHeatmapResult result = engine.process(snapshot());

    EXPECT_EQ(engine.product_id(), "ETH-USD");
    EXPECT_EQ(engine.status(), LiveHeatmapStatus::Live);
    ASSERT_TRUE(result.status_change.has_value());
    EXPECT_EQ(*result.status_change, LiveHeatmapStatus::Live);
    ASSERT_EQ(result.columns.size(), 1U);
    EXPECT_EQ(result.columns[0].index, 0U);
    EXPECT_FALSE(result.columns[0].replaces_existing);
    ASSERT_TRUE(engine.heatmap().resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*engine.heatmap().resolved_price_bin_size(), 0.05);

    const json::object column =
        json::parse(result.columns[0].column_json).as_object();
    EXPECT_DOUBLE_EQ(json::value_to<double>(column.at("mid_price")), 4000.0);
}

TEST(LiveHeatmapEngineTest, ReplacesOpenBucketAndAppendsNewBuckets)
{
    LiveHeatmapEngine engine("ETH-USD");
    engine.process(snapshot());

    const LiveHeatmapResult replacement = engine.process(message(
        "update",
        update("bid", "3999.95", "3.0"),
        1,
        "2026-08-27T19:28:02.050000000Z"
    ));
    ASSERT_EQ(replacement.columns.size(), 1U);
    EXPECT_EQ(replacement.columns[0].index, 0U);
    EXPECT_TRUE(replacement.columns[0].replaces_existing);

    const LiveHeatmapResult appended = engine.process(message(
        "update",
        update("offer", "4000.05", "4.0"),
        2,
        "2026-08-27T19:28:02.250000000Z"
    ));
    ASSERT_EQ(appended.columns.size(), 2U);
    EXPECT_EQ(appended.columns[0].index, 1U);
    EXPECT_EQ(appended.columns[1].index, 2U);
    EXPECT_FALSE(appended.columns[0].replaces_existing);
    EXPECT_FALSE(appended.columns[1].replaces_existing);
}

TEST(LiveHeatmapEngineTest, ReportsGapAndRecoversOnFreshSnapshot)
{
    LiveHeatmapEngine engine("ETH-USD");
    engine.process(snapshot());

    const LiveHeatmapResult gap = engine.process(message(
        "update",
        update("bid", "3999.95", "3.0"),
        2,
        "2026-08-27T19:28:02.200000000Z"
    ));
    ASSERT_TRUE(gap.status_change.has_value());
    EXPECT_EQ(*gap.status_change, LiveHeatmapStatus::Gap);
    EXPECT_TRUE(gap.columns.empty());
    EXPECT_EQ(engine.status(), LiveHeatmapStatus::Gap);

    const LiveHeatmapResult recovered = engine.process(snapshot(
        5,
        "2026-08-27T19:28:02.500000000Z"
    ));
    ASSERT_TRUE(recovered.status_change.has_value());
    EXPECT_EQ(*recovered.status_change, LiveHeatmapStatus::Live);
    ASSERT_EQ(recovered.columns.size(), 1U);
    EXPECT_EQ(recovered.columns[0].index, 1U);
    EXPECT_EQ(engine.status(), LiveHeatmapStatus::Live);
}

TEST(LiveHeatmapEngineTest, RejectsUnexpectedProduct)
{
    LiveHeatmapEngine engine("SOL-USD");

    EXPECT_THROW(engine.process(snapshot()), std::invalid_argument);
}
