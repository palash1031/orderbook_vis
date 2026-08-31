#include "recorder_config.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string_view>

namespace json = boost::json;

TEST(RecorderConfigTest, NormalizesSyntacticallyValidProductIds)
{
    EXPECT_EQ(normalize_product_id("eth-usd"), "ETH-USD");
    EXPECT_EQ(normalize_product_id("  ETH-USD\t"), "ETH-USD");
    EXPECT_EQ(normalize_product_id("1inch-usdc"), "1INCH-USDC");

    // Syntax validation deliberately does not claim that a market is listed.
    EXPECT_EQ(normalize_product_id("BANANA-USD"), "BANANA-USD");
}

TEST(RecorderConfigTest, RejectsMalformedProductIds)
{
    constexpr std::string_view invalid_products[] = {
        "",
        "ETH/USD",
        "ETH USD",
        "../../foo",
        "ETH-",
        "-USD",
        "ETH--USD",
        "ETH_USD"
    };

    for (const std::string_view product : invalid_products)
    {
        EXPECT_THROW(normalize_product_id(product), std::invalid_argument)
            << product;
    }
}

TEST(RecorderConfigTest, DerivesSafeDefaultCapturePaths)
{
    EXPECT_EQ(default_capture_path("BTC-USD"), "btc_usd.jsonl");
    EXPECT_EQ(default_capture_path(" eth-usd "), "eth_usd.jsonl");
    EXPECT_EQ(default_capture_path("SOL-USDC"), "sol_usdc.jsonl");
}

TEST(RecorderConfigTest, BuildsStructuredSingleProductSubscription)
{
    const json::object subscription = json::parse(
        make_level2_subscription(" sol-usd ")
    ).as_object();

    EXPECT_EQ(subscription.at("type").as_string(), "subscribe");
    EXPECT_EQ(subscription.at("channel").as_string(), "level2");
    const json::array& products =
        subscription.at("product_ids").as_array();
    ASSERT_EQ(products.size(), 1U);
    EXPECT_EQ(products.front().as_string(), "SOL-USD");
}

TEST(RecorderConfigTest, BuildsSeparateHeartbeatSubscription)
{
    const json::object subscription = json::parse(
        make_heartbeat_subscription()
    ).as_object();

    EXPECT_EQ(subscription.at("type").as_string(), "subscribe");
    EXPECT_EQ(subscription.at("channel").as_string(), "heartbeats");
    EXPECT_FALSE(subscription.contains("product_ids"));
}

TEST(RecorderConfigTest, ParsesProductAndOutputOptions)
{
    constexpr std::array<std::string_view, 0> no_arguments = {};
    const RecorderOptions defaults = parse_recorder_options(no_arguments);
    EXPECT_EQ(defaults.product_id, "BTC-USD");
    EXPECT_EQ(defaults.output_path, "btc_usd.jsonl");

    constexpr std::array<std::string_view, 2> derived_arguments = {
        "--product", "eth-usd"
    };
    const RecorderOptions derived = parse_recorder_options(
        derived_arguments
    );
    EXPECT_EQ(derived.product_id, "ETH-USD");
    EXPECT_EQ(derived.output_path, "eth_usd.jsonl");

    constexpr std::array<std::string_view, 4> explicit_arguments = {
        "--output", "capture.jsonl", "--product", "sol-usd"
    };
    const RecorderOptions explicit_options = parse_recorder_options(
        explicit_arguments
    );
    EXPECT_EQ(explicit_options.product_id, "SOL-USD");
    EXPECT_EQ(explicit_options.output_path, "capture.jsonl");
}

TEST(RecorderConfigTest, RejectsMissingAndDuplicateOptions)
{
    constexpr std::array<std::string_view, 1> missing_product = {
        "--product"
    };
    EXPECT_THROW(
        parse_recorder_options(missing_product),
        std::invalid_argument
    );

    constexpr std::array<std::string_view, 4> duplicate_product = {
        "--product", "BTC-USD", "--product", "ETH-USD"
    };
    EXPECT_THROW(
        parse_recorder_options(duplicate_product),
        std::invalid_argument
    );
}
