#include "replay_config.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string_view>

TEST(ReplayConfigTest, UsesAutomaticPriceBinsByDefault)
{
    constexpr std::array<std::string_view, 3> arguments = {
        "capture.jsonl", "--heatmap-output", "heatmap.json"
    };
    const ReplayOptions options = parse_replay_options(arguments);

    EXPECT_EQ(options.input_path, "capture.jsonl");
    ASSERT_TRUE(options.heatmap_output_path.has_value());
    EXPECT_EQ(*options.heatmap_output_path, "heatmap.json");
    EXPECT_FALSE(options.heatmap_config.price_bin_size.has_value());
}

TEST(ReplayConfigTest, AcceptsExplicitAutomaticAndManualPriceBins)
{
    constexpr std::array<std::string_view, 2> automatic_arguments = {
        "--price-bin", "auto"
    };
    EXPECT_FALSE(
        parse_replay_options(automatic_arguments)
            .heatmap_config.price_bin_size.has_value()
    );

    constexpr std::array<std::string_view, 2> manual_arguments = {
        "--price-bin", "0.0001"
    };
    const auto manual = parse_replay_options(manual_arguments)
        .heatmap_config.price_bin_size;
    ASSERT_TRUE(manual.has_value());
    EXPECT_DOUBLE_EQ(*manual, 0.0001);
}

TEST(ReplayConfigTest, RejectsInvalidPriceBins)
{
    constexpr std::string_view invalid_values[] = {
        "",
        "0",
        "-1",
        "nan",
        "inf",
        "banana",
        "0.1x"
    };

    for (const std::string_view value : invalid_values)
    {
        EXPECT_THROW(
            parse_price_bin_argument(value),
            std::invalid_argument
        ) << value;
    }
}

TEST(ReplayConfigTest, RejectsDuplicatePriceBinOptions)
{
    constexpr std::array<std::string_view, 4> arguments = {
        "--price-bin", "auto", "--price-bin", "1"
    };

    EXPECT_THROW(parse_replay_options(arguments), std::invalid_argument);
}
