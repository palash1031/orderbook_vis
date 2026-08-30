#include "heatmap_history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

namespace
{
MarketTimestamp at_milliseconds(std::int64_t milliseconds)
{
    return MarketTimestamp{std::chrono::milliseconds{milliseconds}};
}

HeatmapConfig test_config(std::size_t max_columns = 10)
{
    HeatmapConfig config;
    config.time_bucket = std::chrono::milliseconds{100};
    config.price_bin_size = 1.0;
    config.price_bin_count = 5;
    config.max_columns = max_columns;
    return config;
}

OrderBook populated_book()
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 99.2, 1.0);
    book.apply_update(BookSide::Bid, 99.8, 2.0);
    book.apply_update(BookSide::Offer, 100.2, 3.0);
    book.apply_update(BookSide::Offer, 101.7, 4.0);
    return book;
}

OrderBook book_at_midpoint(double midpoint)
{
    const double half_spread = midpoint * 0.00001;
    OrderBook book;
    book.apply_update(BookSide::Bid, midpoint - half_spread, 1.0);
    book.apply_update(BookSide::Offer, midpoint + half_spread, 1.0);
    return book;
}
}

TEST(HeatmapHistoryTest, AggregatesDepthIntoPriceBins)
{
    HeatmapHistory history(test_config());
    const OrderBook book = populated_book();

    ASSERT_TRUE(history.sample(at_milliseconds(42), book));
    ASSERT_EQ(history.columns().size(), 1U);

    const HeatmapColumn& column = history.columns().front();
    EXPECT_EQ(column.timestamp, at_milliseconds(0));
    EXPECT_DOUBLE_EQ(column.first_price, 98.0);
    EXPECT_DOUBLE_EQ(column.mid_price, 100.0);
    ASSERT_EQ(column.bid_quantities.size(), 5U);
    ASSERT_EQ(column.ask_quantities.size(), 5U);
    EXPECT_DOUBLE_EQ(column.bid_quantities[1], 3.0);
    EXPECT_DOUBLE_EQ(column.ask_quantities[2], 3.0);
    EXPECT_DOUBLE_EQ(column.ask_quantities[3], 4.0);
    EXPECT_DOUBLE_EQ(column.price_at(3), 101.0);
}

TEST(HeatmapHistoryTest, ReplacesPartialColumnWithinSameTimeBucket)
{
    HeatmapHistory history(test_config());
    OrderBook book = populated_book();
    history.sample(at_milliseconds(10), book);

    book.apply_update(BookSide::Bid, 99.2, 5.0);
    history.sample(at_milliseconds(99), book);

    ASSERT_EQ(history.columns().size(), 1U);
    EXPECT_DOUBLE_EQ(history.columns().front().bid_quantities[1], 7.0);
}

TEST(HeatmapHistoryTest, CarriesLastStateThroughSkippedBuckets)
{
    HeatmapHistory history(test_config());
    OrderBook book = populated_book();
    history.sample(at_milliseconds(0), book);

    book.apply_update(BookSide::Offer, 100.2, 10.0);
    history.sample(at_milliseconds(350), book);

    ASSERT_EQ(history.columns().size(), 4U);
    EXPECT_EQ(history.columns()[0].timestamp, at_milliseconds(0));
    EXPECT_EQ(history.columns()[1].timestamp, at_milliseconds(100));
    EXPECT_EQ(history.columns()[2].timestamp, at_milliseconds(200));
    EXPECT_EQ(history.columns()[3].timestamp, at_milliseconds(300));
    EXPECT_DOUBLE_EQ(history.columns()[1].ask_quantities[2], 3.0);
    EXPECT_DOUBLE_EQ(history.columns()[2].ask_quantities[2], 3.0);
    EXPECT_DOUBLE_EQ(history.columns()[3].ask_quantities[2], 10.0);
}

TEST(HeatmapHistoryTest, EvictsOldColumnsAndBoundsLargeGaps)
{
    HeatmapHistory history(test_config(3));
    OrderBook book = populated_book();
    history.sample(at_milliseconds(0), book);
    history.sample(at_milliseconds(350), book);

    ASSERT_EQ(history.columns().size(), 3U);
    EXPECT_EQ(history.columns()[0].timestamp, at_milliseconds(100));
    EXPECT_EQ(history.columns()[1].timestamp, at_milliseconds(200));
    EXPECT_EQ(history.columns()[2].timestamp, at_milliseconds(300));

    history.sample(at_milliseconds(10'000), book);

    ASSERT_EQ(history.columns().size(), 3U);
    EXPECT_EQ(history.columns()[0].timestamp, at_milliseconds(9'800));
    EXPECT_EQ(history.columns()[1].timestamp, at_milliseconds(9'900));
    EXPECT_EQ(history.columns()[2].timestamp, at_milliseconds(10'000));
}

TEST(HeatmapHistoryTest, LeavesUnknownTimeBlankAfterDiscontinuity)
{
    HeatmapHistory history(test_config());
    OrderBook book = populated_book();
    history.sample(at_milliseconds(0), book);

    history.mark_discontinuity();
    book.apply_update(BookSide::Offer, 100.2, 10.0);
    history.sample(at_milliseconds(350), book);

    ASSERT_EQ(history.columns().size(), 2U);
    EXPECT_EQ(history.columns()[0].timestamp, at_milliseconds(0));
    EXPECT_EQ(history.columns()[1].timestamp, at_milliseconds(300));
    EXPECT_DOUBLE_EQ(history.columns()[1].ask_quantities[2], 10.0);
}

TEST(HeatmapHistoryTest, WaitsForBothSidesOfBook)
{
    HeatmapHistory history(test_config());
    OrderBook book;
    book.apply_update(BookSide::Bid, 99.0, 1.0);

    EXPECT_FALSE(history.sample(at_milliseconds(0), book));
    EXPECT_TRUE(history.columns().empty());

    book.apply_update(BookSide::Offer, 101.0, 1.0);
    EXPECT_TRUE(history.sample(at_milliseconds(1), book));
    EXPECT_EQ(history.columns().size(), 1U);
}

TEST(HeatmapHistoryTest, AutomaticResolutionWaitsForTwoSidedBook)
{
    HeatmapHistory history;
    OrderBook book;
    book.apply_update(BookSide::Bid, 79'999.0, 1.0);

    EXPECT_FALSE(history.resolved_price_bin_size().has_value());
    EXPECT_FALSE(history.sample(at_milliseconds(0), book));
    EXPECT_FALSE(history.resolved_price_bin_size().has_value());

    book.apply_update(BookSide::Offer, 80'001.0, 1.0);
    EXPECT_TRUE(history.sample(at_milliseconds(1), book));
    ASSERT_TRUE(history.resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*history.resolved_price_bin_size(), 1.0);
}

TEST(HeatmapHistoryTest, AutomaticResolutionScalesAcrossProducts)
{
    EXPECT_DOUBLE_EQ(automatic_price_bin_size(80'000.0, 201), 1.0);
    EXPECT_DOUBLE_EQ(automatic_price_bin_size(4'000.0, 201), 0.05);
    EXPECT_DOUBLE_EQ(automatic_price_bin_size(200.0, 201), 0.0025);
    EXPECT_DOUBLE_EQ(automatic_price_bin_size(0.20, 201), 0.0000025);
}

TEST(HeatmapHistoryTest, AutomaticResolutionFreezesAcrossDiscontinuities)
{
    HeatmapHistory history;
    history.sample(at_milliseconds(0), book_at_midpoint(80'000.0));
    ASSERT_TRUE(history.resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*history.resolved_price_bin_size(), 1.0);

    history.mark_discontinuity();
    history.sample(at_milliseconds(100), book_at_midpoint(4'000.0));

    ASSERT_TRUE(history.resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*history.resolved_price_bin_size(), 1.0);
    ASSERT_EQ(history.columns().size(), 2U);
    EXPECT_DOUBLE_EQ(history.columns().back().price_bin_size, 1.0);
}

TEST(HeatmapHistoryTest, ClearResetsAutomaticResolution)
{
    HeatmapHistory history;
    history.sample(at_milliseconds(100), book_at_midpoint(80'000.0));
    ASSERT_TRUE(history.resolved_price_bin_size().has_value());

    history.clear();

    EXPECT_TRUE(history.columns().empty());
    EXPECT_FALSE(history.resolved_price_bin_size().has_value());
    history.sample(at_milliseconds(0), book_at_midpoint(4'000.0));
    ASSERT_TRUE(history.resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*history.resolved_price_bin_size(), 0.05);
}

TEST(HeatmapHistoryTest, ClearPreservesManualResolution)
{
    HeatmapHistory history(test_config());
    history.sample(at_milliseconds(100), populated_book());
    history.clear();

    ASSERT_TRUE(history.resolved_price_bin_size().has_value());
    EXPECT_DOUBLE_EQ(*history.resolved_price_bin_size(), 1.0);
}

TEST(HeatmapHistoryTest, RejectsOutOfOrderObservations)
{
    HeatmapHistory history(test_config());
    const OrderBook book = populated_book();
    history.sample(at_milliseconds(200), book);

    EXPECT_THROW(
        history.sample(at_milliseconds(199), book),
        std::invalid_argument
    );
}

TEST(HeatmapHistoryTest, HandlesDecimalPriceBinBoundaries)
{
    HeatmapConfig config = test_config();
    config.price_bin_size = 0.01;
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.00, 1.0);
    book.apply_update(BookSide::Offer, 100.01, 2.0);
    HeatmapHistory history(config);

    history.sample(at_milliseconds(0), book);

    const HeatmapColumn& column = history.columns().front();
    EXPECT_DOUBLE_EQ(column.bid_quantities[2], 1.0);
    EXPECT_DOUBLE_EQ(column.ask_quantities[3], 2.0);
}

TEST(HeatmapHistoryTest, ValidatesConfigurationAndIndices)
{
    HeatmapConfig config = test_config();
    config.price_bin_count = 4;
    EXPECT_THROW(HeatmapHistory history(config), std::invalid_argument);

    config = test_config();
    config.time_bucket = std::chrono::nanoseconds{0};
    EXPECT_THROW(HeatmapHistory history(config), std::invalid_argument);

    config = test_config();
    config.price_bin_size = 0.0;
    EXPECT_THROW(HeatmapHistory history(config), std::invalid_argument);

    HeatmapHistory history(test_config());
    const OrderBook book = populated_book();
    history.sample(at_milliseconds(0), book);
    EXPECT_THROW(history.columns().front().price_at(5), std::out_of_range);
}

TEST(HeatmapHistoryTest, ClearResetsHistoryAndTimestampOrdering)
{
    HeatmapHistory history(test_config());
    const OrderBook book = populated_book();
    history.sample(at_milliseconds(500), book);

    history.clear();

    EXPECT_TRUE(history.columns().empty());
    EXPECT_TRUE(history.sample(at_milliseconds(0), book));
}
