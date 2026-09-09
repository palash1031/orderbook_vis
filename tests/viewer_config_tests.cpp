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
    EXPECT_EQ(options.bind_address, "127.0.0.1");
    EXPECT_EQ(options.port, 8'080);
    EXPECT_FALSE(options.public_demo);
}

TEST(ViewerConfigTest, AcceptsPublicDemoAndNumericBindAddress)
{
    constexpr std::array<std::string_view, 4> arguments{
        "--live", "--public-demo", "--bind", "0.0.0.0"
    };
    const ViewerOptions options = parse_viewer_options(arguments);

    EXPECT_TRUE(options.public_demo);
    EXPECT_EQ(options.bind_address, "0.0.0.0");
}

TEST(ViewerConfigTest, UsesEnvironmentPortWhenCliPortIsAbsent)
{
    constexpr std::array<std::string_view, 1> arguments{"--live"};
    const ViewerOptions options = parse_viewer_options(
        arguments,
        "web",
        std::string_view{"10000"}
    );

    EXPECT_EQ(options.port, 10'000);
}

TEST(ViewerConfigTest, CliPortOverridesEnvironmentPort)
{
    constexpr std::array<std::string_view, 3> arguments{
        "--live", "--port", "8081"
    };
    const ViewerOptions options = parse_viewer_options(
        arguments,
        "web",
        std::string_view{"10000"}
    );

    EXPECT_EQ(options.port, 8'081);
}

TEST(ViewerConfigTest, CliPortOverridesMalformedEnvironmentPort)
{
    constexpr std::array<std::string_view, 3> arguments{
        "--live", "--port", "8081"
    };
    const ViewerOptions options = parse_viewer_options(
        arguments,
        "web",
        std::string_view{"not-a-port"}
    );

    EXPECT_EQ(options.port, 8'081);
}

TEST(ViewerConfigTest, RejectsInvalidEnvironmentPorts)
{
    constexpr std::array<std::string_view, 1> arguments{"--live"};

    for (const std::string_view port : {
             std::string_view{""},
             std::string_view{"0"},
             std::string_view{"65536"},
             std::string_view{"8080junk"},
             std::string_view{"-1"}
         })
    {
        EXPECT_THROW(
            parse_viewer_options(arguments, "web", port),
            std::invalid_argument
        ) << port;
    }
}

TEST(ViewerConfigTest, RejectsHostnamesAndMalformedBindAddresses)
{
    constexpr std::array<std::string_view, 2> hostname{
        "--bind", "localhost"
    };
    EXPECT_THROW(parse_viewer_options(hostname), std::invalid_argument);

    constexpr std::array<std::string_view, 2> malformed{
        "--bind", "999.0.0.1"
    };
    EXPECT_THROW(parse_viewer_options(malformed), std::invalid_argument);
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
