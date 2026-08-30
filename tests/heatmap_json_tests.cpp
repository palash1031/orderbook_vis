#include "heatmap_json.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>

namespace json = boost::json;

TEST(HeatmapJsonTest, WritesVersionedBrowserSafeDocument)
{
    HeatmapConfig config;
    config.time_bucket = std::chrono::milliseconds{100};
    config.price_bin_size = 1.0;
    config.price_bin_count = 3;
    config.max_columns = 2;

    OrderBook book;
    book.apply_update(BookSide::Bid, 99.0, 1.5);
    book.apply_update(BookSide::Offer, 101.0, 2.5);

    HeatmapHistory heatmap(config);
    heatmap.sample(
        MarketTimestamp{std::chrono::milliseconds{1'750'000'000'123LL}},
        book
    );

    std::ostringstream output;
    write_heatmap_json(output, R"(BTC-"USD)", heatmap);

    const json::object document = json::parse(output.str()).as_object();
    EXPECT_EQ(document.at("schema_version").as_int64(), 1);
    EXPECT_EQ(document.at("product_id").as_string(), R"(BTC-"USD)");

    const json::object& parsed_config = document.at("config").as_object();
    EXPECT_EQ(parsed_config.at("time_bucket_ns").as_int64(), 100'000'000);
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(parsed_config.at("price_bin_size")),
        1.0
    );
    EXPECT_EQ(parsed_config.at("price_bin_count").as_int64(), 3);
    EXPECT_EQ(parsed_config.at("max_columns").as_int64(), 2);

    const json::array& columns = document.at("columns").as_array();
    ASSERT_EQ(columns.size(), 1U);
    const json::object& column = columns.front().as_object();
    EXPECT_TRUE(column.at("timestamp_ns").is_string());
    EXPECT_EQ(column.at("timestamp_ms").as_int64(), 1'750'000'000'100LL);
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(column.at("mid_price")),
        100.0
    );

    const json::array& bids = column.at("bids").as_array();
    const json::array& asks = column.at("asks").as_array();
    ASSERT_EQ(bids.size(), 3U);
    ASSERT_EQ(asks.size(), 3U);
    EXPECT_DOUBLE_EQ(json::value_to<double>(bids[0]), 1.5);
    EXPECT_DOUBLE_EQ(json::value_to<double>(asks[2]), 2.5);
}

TEST(HeatmapJsonTest, ExportsResolvedNumericAutomaticPriceBin)
{
    HeatmapHistory heatmap;
    OrderBook book;
    book.apply_update(BookSide::Bid, 3'999.9, 1.0);
    book.apply_update(BookSide::Offer, 4'000.1, 1.0);
    ASSERT_TRUE(heatmap.sample(MarketTimestamp{}, book));

    std::ostringstream output;
    write_heatmap_json(output, "ETH-USD", heatmap);

    const json::object document = json::parse(output.str()).as_object();
    const json::value& price_bin_size =
        document.at("config").as_object().at("price_bin_size");
    EXPECT_TRUE(price_bin_size.is_number());
    EXPECT_DOUBLE_EQ(json::value_to<double>(price_bin_size), 0.05);
}

TEST(HeatmapJsonTest, RejectsUnresolvedAutomaticPriceBin)
{
    HeatmapHistory heatmap;
    std::ostringstream output;

    EXPECT_THROW(
        write_heatmap_json(output, "ETH-USD", heatmap),
        std::runtime_error
    );
}
