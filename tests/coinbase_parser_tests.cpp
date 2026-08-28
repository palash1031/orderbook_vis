#include "coinbase_parser.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace
{
std::string make_update(
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

std::string make_event(
    const std::string& type,
    const std::string& updates)
{
    return
        R"({"type":")" + type
        + R"(","product_id":"BTC-USD","updates":[)" + updates
        + R"(]})";
}

std::string make_level2_message(
    const std::string& events,
    const std::string& sequence_num = "1")
{
    return
        R"({"channel":"l2_data","timestamp":"...","sequence_num":)"
        + sequence_num
        + R"(,"events":[)" + events
        + R"(]})";
}
}

TEST(CoinbaseParserTest, IgnoresSubscriptionsMessage)
{
    const std::string message = R"({
        "channel":"subscriptions",
        "timestamp":"...",
        "sequence_num":3,
        "events":[
            {"subscriptions":{"level2":["BTC-USD"]}}
        ]
    })";

    EXPECT_FALSE(CoinbaseParser::parse(message).has_value());
}

TEST(CoinbaseParserTest, ParsesSnapshot)
{
    const std::string updates =
        make_update("bid", "80000.01", "1.5") + ","
        + make_update("offer", "80000.02", "2.0");
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(make_event("snapshot", updates), "0")
    );

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, BookEventType::Snapshot);
    EXPECT_EQ(parsed->sequence_num, 0U);
    ASSERT_EQ(parsed->updates.size(), 2U);
    EXPECT_EQ(parsed->updates[0].side, "bid");
    EXPECT_DOUBLE_EQ(parsed->updates[0].price, 80000.01);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 1.5);
    EXPECT_EQ(parsed->updates[1].side, "offer");
    EXPECT_DOUBLE_EQ(parsed->updates[1].price, 80000.02);
    EXPECT_DOUBLE_EQ(parsed->updates[1].quantity, 2.0);
}

TEST(CoinbaseParserTest, ParsesUpdate)
{
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(
            make_event("update", make_update("bid", "99.5", "3.25")),
            "42"
        )
    );

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, BookEventType::Update);
    EXPECT_EQ(parsed->sequence_num, 42U);
    ASSERT_EQ(parsed->updates.size(), 1U);
    EXPECT_EQ(parsed->updates[0].side, "bid");
    EXPECT_DOUBLE_EQ(parsed->updates[0].price, 99.5);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 3.25);
}

TEST(CoinbaseParserTest, ParsesMultipleUpdates)
{
    const std::string updates =
        make_update("bid", "100.0", "1.0") + ","
        + make_update("offer", "101.0", "2.0") + ","
        + make_update("bid", "99.0", "3.0");
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(make_event("update", updates))
    );

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->updates.size(), 3U);
    EXPECT_DOUBLE_EQ(parsed->updates[0].price, 100.0);
    EXPECT_DOUBLE_EQ(parsed->updates[1].price, 101.0);
    EXPECT_DOUBLE_EQ(parsed->updates[2].price, 99.0);
}

TEST(CoinbaseParserTest, ParsesMultipleEvents)
{
    const std::string events =
        make_event("update", make_update("bid", "100.0", "1.0"))
        + ","
        + make_event("update", make_update("offer", "101.0", "2.0"));
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(events)
    );

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, BookEventType::Update);
    ASSERT_EQ(parsed->updates.size(), 2U);
    EXPECT_EQ(parsed->updates[0].side, "bid");
    EXPECT_EQ(parsed->updates[1].side, "offer");
}

TEST(CoinbaseParserTest, ZeroQuantityParsesCorrectly)
{
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(
            make_event("update", make_update("bid", "100.0", "0"))
        )
    );

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->updates.size(), 1U);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 0.0);
}

TEST(CoinbaseParserTest, MalformedJsonThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(R"({"channel":)"),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingChannelThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(R"({"sequence_num":1,"events":[]})"),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingSequenceNumberThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(R"({"channel":"l2_data","events":[]})"),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingEventsThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            R"({"channel":"l2_data","sequence_num":1})"
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingUpdatesThrowsForL2BookEvent)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(R"({"type":"update"})")
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingSideThrows)
{
    const std::string update =
        R"({"price_level":"100.0","new_quantity":"1.0"})";

    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(make_event("update", update))
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingPriceLevelThrows)
{
    const std::string update =
        R"({"side":"bid","new_quantity":"1.0"})";

    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(make_event("update", update))
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingNewQuantityThrows)
{
    const std::string update =
        R"({"side":"bid","price_level":"100.0"})";

    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(make_event("update", update))
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, UnsupportedEventTypeThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "something_else",
                    make_update("bid", "100.0", "1.0")
                )
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, InvalidNumericPriceThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "not-a-number", "1.0")
                )
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, InvalidNumericQuantityThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "100.0", "not-a-number")
                )
            )
        ),
        std::invalid_argument
    );
}
