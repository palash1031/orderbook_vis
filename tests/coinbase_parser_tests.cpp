#include "coinbase_parser.hpp"

#include <gtest/gtest.h>

#include <chrono>
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
    const std::string& updates,
    const std::string& product_id = "BTC-USD")
{
    return
        R"({"type":")" + type
        + R"(","product_id":")" + product_id
        + R"(","updates":[)" + updates
        + R"(]})";
}

std::string make_level2_message(
    const std::string& events,
    const std::string& sequence_num = "1",
    const std::string& timestamp = "2026-08-27T19:28:02.592102664Z")
{
    return
        R"({"channel":"l2_data","timestamp":")" + timestamp
        + R"(","sequence_num":)"
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
    EXPECT_EQ(parsed->updates[0].side, BookSide::Bid);
    EXPECT_DOUBLE_EQ(parsed->updates[0].price, 80000.01);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 1.5);
    EXPECT_EQ(parsed->updates[1].side, BookSide::Offer);
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
    EXPECT_EQ(parsed->updates[0].side, BookSide::Bid);
    EXPECT_DOUBLE_EQ(parsed->updates[0].price, 99.5);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 3.25);
}

TEST(CoinbaseParserTest, PreservesProductAndNanosecondTimestamp)
{
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(
            make_event(
                "update",
                make_update("bid", "99.5", "3.25"),
                "ETH-USD"
            )
        )
    );

    const MarketTimestamp expected_timestamp =
        std::chrono::sys_days{
            std::chrono::year{2026}
            / std::chrono::month{8}
            / std::chrono::day{27}
        }
        + std::chrono::hours{19}
        + std::chrono::minutes{28}
        + std::chrono::seconds{2}
        + std::chrono::nanoseconds{592102664};

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->product_id, "ETH-USD");
    EXPECT_EQ(parsed->timestamp, expected_timestamp);
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
    EXPECT_EQ(parsed->updates[0].side, BookSide::Bid);
    EXPECT_EQ(parsed->updates[1].side, BookSide::Offer);
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
            R"({
                "channel":"l2_data",
                "timestamp":"2026-08-27T19:28:02Z",
                "sequence_num":1
            })"
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingTimestampThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            R"({
                "channel":"l2_data",
                "sequence_num":1,
                "events":[]
            })"
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MissingUpdatesThrowsForL2BookEvent)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                R"({"type":"update","product_id":"BTC-USD"})"
            )
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

TEST(CoinbaseParserTest, ParsesScientificNotation)
{
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(
            make_event(
                "update",
                make_update("bid", "80000.01", "1e-8")
            )
        )
    );

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->updates.size(), 1U);
    EXPECT_DOUBLE_EQ(parsed->updates[0].quantity, 1e-8);
}

TEST(CoinbaseParserTest, PartiallyParsedNumericValueThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "80000x", "1.0")
                )
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, EmptyNumericValueThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "80000.01", "")
                )
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, UnsupportedSideThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("unknown", "80000.01", "1.0")
                )
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, InvalidTimestampThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "80000.01", "1.0")
                ),
                "1",
                "2026-02-30T19:28:02Z"
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, ScalesShortFractionalTimestampToNanoseconds)
{
    const auto parsed = CoinbaseParser::parse(
        make_level2_message(
            make_event(
                "update",
                make_update("bid", "80000.01", "1.0")
            ),
            "1",
            "1970-01-01T00:00:00.5Z"
        )
    );

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(
        parsed->timestamp,
        MarketTimestamp{std::chrono::milliseconds{500}}
    );
}

TEST(CoinbaseParserTest, InvalidFractionalTimestampThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "80000.01", "1.0")
                ),
                "1",
                "2026-08-27T19:28:02.12xZ"
            )
        ),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, MixedProductIdsThrow)
{
    const std::string events =
        make_event(
            "update",
            make_update("bid", "80000.01", "1.0"),
            "BTC-USD"
        )
        + ","
        + make_event(
            "update",
            make_update("offer", "80000.02", "1.0"),
            "ETH-USD"
        );

    EXPECT_THROW(
        CoinbaseParser::parse(make_level2_message(events)),
        std::invalid_argument
    );
}

TEST(CoinbaseParserTest, NonFiniteNumericValueThrows)
{
    EXPECT_THROW(
        CoinbaseParser::parse(
            make_level2_message(
                make_event(
                    "update",
                    make_update("bid", "80000.01", "nan")
                )
            )
        ),
        std::invalid_argument
    );
}
