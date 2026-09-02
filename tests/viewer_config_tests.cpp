#include "viewer_config.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string_view>

TEST(ViewerConfigTest, KeepsCoinbaseAsLiveVenueDefault)
{
    constexpr std::array<std::string_view, 1> arguments{"--live"};
    const ViewerOptions options = parse_viewer_options(arguments);

    EXPECT_TRUE(options.live);
    EXPECT_EQ(options.venue, Venue::Coinbase);
    EXPECT_EQ(options.product_id, "BTC-USD");
}

TEST(ViewerConfigTest, AcceptsKrakenVenueAndCanonicalUniProduct)
{
    constexpr std::array<std::string_view, 5> arguments{
        "--live", "--venue", "kraken", "--product", " uni-usd "
    };
    const ViewerOptions options = parse_viewer_options(arguments);

    EXPECT_EQ(options.venue, Venue::Kraken);
    EXPECT_EQ(options.product_id, "UNI-USD");
}

TEST(ViewerConfigTest, RejectsInvalidDuplicateAndReplayVenueOptions)
{
    constexpr std::array<std::string_view, 3> invalid{
        "--live", "--venue", "gemini"
    };
    EXPECT_THROW(parse_viewer_options(invalid), std::invalid_argument);

    constexpr std::array<std::string_view, 5> duplicate{
        "--live", "--venue", "kraken", "--venue", "coinbase"
    };
    EXPECT_THROW(parse_viewer_options(duplicate), std::invalid_argument);

    constexpr std::array<std::string_view, 2> replay{
        "--venue", "kraken"
    };
    EXPECT_THROW(parse_viewer_options(replay), std::invalid_argument);
}
