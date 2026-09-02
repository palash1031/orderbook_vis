#include "kraken_adapter.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace json = boost::json;

namespace
{
constexpr std::string_view instrument_fixture = R"({
  "channel":"instrument",
  "type":"snapshot",
  "data":{
    "assets":[],
    "pairs":[
      {"symbol":"UNI/USD","base":"UNI","quote":"USD","status":"online"},
      {"symbol":"ETH/USD","base":"ETH","quote":"USD","status":"maintenance"}
    ]
  }
})";

std::string read_fixture(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path()
        / "fixtures"
        / name;
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        throw std::runtime_error("Failed to open fixture: " + path.string());
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<std::string> read_fixture_lines(std::string_view name)
{
    const std::string contents = read_fixture(name);
    std::istringstream input(contents);
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(input, line))
    {
        if (!line.empty())
        {
            lines.push_back(std::move(line));
        }
    }

    return lines;
}

constexpr std::string_view ten_level_snapshot_fixture = R"({
  "channel":"book","type":"snapshot","data":[{
    "symbol":"UNI/USD",
    "asks":[
      {"price":"101","qty":"1"},{"price":"102","qty":"1"},
      {"price":"103","qty":"1"},{"price":"104","qty":"1"},
      {"price":"105","qty":"1"},{"price":"106","qty":"1"},
      {"price":"107","qty":"1"},{"price":"108","qty":"1"},
      {"price":"109","qty":"1"},{"price":"110","qty":"1"}
    ],
    "bids":[
      {"price":"100","qty":"1"},{"price":"99","qty":"1"},
      {"price":"98","qty":"1"},{"price":"97","qty":"1"},
      {"price":"96","qty":"1"},{"price":"95","qty":"1"},
      {"price":"94","qty":"1"},{"price":"93","qty":"1"},
      {"price":"92","qty":"1"},{"price":"91","qty":"1"}
    ],
    "checksum":3289024467,
    "timestamp":"2026-09-02T12:00:01Z"
  }]
})";

constexpr std::string_view post_batch_truncation_fixture = R"({
  "channel":"book","type":"update","data":[{
    "symbol":"UNI/USD","asks":[],
    "bids":[
      {"price":"101","qty":"2"},
      {"price":"102","qty":"2"},
      {"price":"91","qty":"9"}
    ],
    "checksum":3834915759,
    "timestamp":"2026-09-02T12:00:01.1Z"
  }]
})";
}

TEST(KrakenCatalogTest, TranslatesUniNativeSymbolFromInstrumentMetadata)
{
    const KrakenInstrumentCatalog catalog =
        KrakenInstrumentCatalog::parse(instrument_fixture);

    EXPECT_EQ(
        catalog.native_symbol(Product("UNI", "USD")),
        std::optional<std::string>{"UNI/USD"}
    );
    EXPECT_EQ(
        catalog.canonical_product("UNI/USD"),
        std::optional<Product>{Product("UNI", "USD")}
    );
    EXPECT_FALSE(catalog.native_symbol(Product("ETH", "USD")).has_value());
}

TEST(KrakenCatalogTest, RejectsInvalidAndAmbiguousPairMetadata)
{
    EXPECT_THROW(
        KrakenInstrumentCatalog::parse(
            R"({"channel":"instrument","type":"snapshot","data":{"pairs":[{"symbol":"UNI/USD","quote":"USD","status":"online"}]}})"
        ),
        std::invalid_argument
    );
    EXPECT_THROW(
        KrakenInstrumentCatalog::parse(
            R"({"channel":"instrument","type":"snapshot","data":{"pairs":[{"symbol":"UNI/USD","base":"UNI","quote":"USD","status":"online"},{"symbol":"UNI2/USD","base":"UNI","quote":"USD","status":"online"}]}})"
        ),
        std::invalid_argument
    );
    EXPECT_THROW(
        KrakenInstrumentCatalog::parse(
            R"({"channel":"book","type":"snapshot","data":{"pairs":[]}})"
        ),
        std::invalid_argument
    );
}

TEST(KrakenSubscriptionTest, UsesOfficialDepthSetAndCanonicalNativeBoundary)
{
    constexpr std::array<std::size_t, 5> supported{10, 25, 100, 500, 1000};

    for (const std::size_t depth : supported)
    {
        EXPECT_TRUE(is_supported_kraken_book_depth(depth));
    }

    EXPECT_FALSE(is_supported_kraken_book_depth(0));
    EXPECT_FALSE(is_supported_kraken_book_depth(50));
    EXPECT_FALSE(is_supported_kraken_book_depth(501));

    const json::object instrument =
        json::parse(make_kraken_instrument_subscription()).as_object();
    EXPECT_EQ(instrument.at("method").as_string(), "subscribe");
    EXPECT_EQ(
        instrument.at("params").as_object().at("channel").as_string(),
        "instrument"
    );

    const json::object book =
        json::parse(make_kraken_book_subscription("UNI/USD")).as_object();
    const json::object& params = book.at("params").as_object();
    EXPECT_EQ(params.at("channel").as_string(), "book");
    EXPECT_EQ(params.at("depth").as_int64(), 500);
    EXPECT_TRUE(params.at("snapshot").as_bool());
    EXPECT_EQ(params.at("symbol").as_array().front().as_string(), "UNI/USD");

    EXPECT_THROW(
        make_kraken_book_subscription("UNI-USD"),
        std::invalid_argument
    );
    EXPECT_THROW(
        make_kraken_book_subscription("UNI/USD", 50),
        std::invalid_argument
    );
}

TEST(KrakenBookAdapterTest, MatchesKrakenPublishedChecksumVector)
{
    KrakenBookAdapter adapter(Product("BTC", "USD"), "BTC/USD", 10);

    const std::optional<TrustedBookEvent> event = adapter.process(
        read_fixture("kraken_v2_checksum_snapshot.json")
    );

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(event->market, (MarketKey{Venue::Kraken, Product("BTC", "USD")}));
    EXPECT_EQ(adapter.status(), VenueMarketStatus::Live);
    EXPECT_EQ(adapter.checksum(), 3310070434U);
    EXPECT_EQ(
        adapter.checksum_input(),
        "452852100000452864154571953452866154571109452896154560911"
        "45290215890660452918154553491452947445474945296135380000"
        "45297599455424529951877282745283510000000452834154582015"
        "45282110000000452810100000004528031545925864527907990000"
        "45277633101034527753000000045277315460273745276615445238"
    );
}

TEST(KrakenBookAdapterTest, ReconstructsUniSnapshotAndAtomicNativeOrderUpdate)
{
    const std::vector<std::string> messages = read_fixture_lines(
        "kraken_uni_book.jsonl"
    );
    ASSERT_EQ(messages.size(), 2U);
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);

    const std::optional<TrustedBookEvent> snapshot = adapter.process(messages[0]);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->book.has_value());
    EXPECT_EQ(snapshot->type, TrustedBookEventType::Snapshot);
    EXPECT_DOUBLE_EQ(*snapshot->book->best_bid(), 4.50);
    EXPECT_DOUBLE_EQ(*snapshot->book->best_ask(), 4.51);
    EXPECT_EQ(adapter.checksum(), 2090218919U);

    const std::optional<TrustedBookEvent> update = adapter.process(messages[1]);
    ASSERT_TRUE(update.has_value());
    ASSERT_TRUE(update->book.has_value());
    EXPECT_EQ(update->type, TrustedBookEventType::Update);
    EXPECT_DOUBLE_EQ(*update->book->best_ask(), 4.52);
    EXPECT_DOUBLE_EQ(update->book->asks().at(4.53), 6.0);
    EXPECT_FALSE(update->book->bids().contains(4.48));
    EXPECT_EQ(adapter.checksum(), 387929384U);
}

TEST(KrakenBookAdapterTest, TruncatesOnlyAfterCompleteUpdateBatch)
{
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 10);
    ASSERT_EQ(
        adapter.process(ten_level_snapshot_fixture)->type,
        TrustedBookEventType::Snapshot
    );

    const std::optional<TrustedBookEvent> event = adapter.process(
        post_batch_truncation_fixture
    );

    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->book.has_value());
    EXPECT_EQ(event->book->bid_levels(), 10U);
    EXPECT_DOUBLE_EQ(*event->book->best_bid(), 102.0);
    EXPECT_TRUE(event->book->bids().contains(93.0));
    EXPECT_FALSE(event->book->bids().contains(92.0));
    EXPECT_FALSE(event->book->bids().contains(91.0));
}

TEST(KrakenBookAdapterTest, InvalidatesOnChecksumFailureAndNeedsFreshSnapshot)
{
    const std::vector<std::string> messages = read_fixture_lines(
        "kraken_uni_book.jsonl"
    );
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
    ASSERT_TRUE(adapter.process(messages[0])->book.has_value());

    const std::optional<TrustedBookEvent> invalid = adapter.process(R"({
      "channel":"book","type":"update","data":[{
        "symbol":"UNI/USD","asks":[],
        "bids":[{"price":"4.50","qty":"9"}],
        "checksum":1,"timestamp":"2026-09-02T12:00:02Z"
      }]
    })");
    ASSERT_TRUE(invalid.has_value());
    EXPECT_EQ(invalid->type, TrustedBookEventType::Invalidated);
    EXPECT_FALSE(invalid->book.has_value());
    EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale);

    EXPECT_FALSE(adapter.process(messages[1]).has_value());
    EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale);

    const std::optional<TrustedBookEvent> recovered = adapter.process(messages[0]);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->type, TrustedBookEventType::Snapshot);
    EXPECT_TRUE(recovered->book.has_value());
    EXPECT_EQ(adapter.status(), VenueMarketStatus::Live);
}

TEST(KrakenBookAdapterTest, InvalidatesMalformedNonFiniteAndNegativeLevels)
{
    constexpr std::array<std::string_view, 5> invalid_levels{
        R"({"price":"4.50","qty":"NaN"})",
        R"({"price":"inf","qty":"1"})",
        R"({"price":"-4.50","qty":"1"})",
        R"({"price":"4.50","qty":"-1"})",
        R"({"price":"4.50"})"
    };

    for (const std::string_view level : invalid_levels)
    {
        KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
        const std::string message =
            R"({"channel":"book","type":"snapshot","data":[{"symbol":"UNI/USD","asks":[])"
            + std::string(level)
            + R"(],"bids":[{"price":"4.49","qty":"1"}],"checksum":0,"timestamp":"2026-09-02T12:00:00Z"}]})";

        const std::optional<TrustedBookEvent> event = adapter.process(message);
        ASSERT_TRUE(event.has_value()) << level;
        EXPECT_EQ(event->type, TrustedBookEventType::Invalidated) << level;
        EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale) << level;
    }
}

TEST(KrakenBookAdapterTest, RejectsZeroQuantitySnapshotLevels)
{
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 10);
    const auto event = adapter.process(R"({
      "channel":"book","type":"snapshot","data":[{
        "symbol":"UNI/USD",
        "asks":[
          {"price":"4.51","qty":"1"},
          {"price":"4.52","qty":"0"}
        ],
        "bids":[{"price":"4.50","qty":"1"}],
        "checksum":518533985,
        "timestamp":"2026-09-02T12:00:00Z"
      }]
    })");

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, TrustedBookEventType::Invalidated);
    EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale);
}

TEST(KrakenBookAdapterTest, PreservesUnquotedWireDecimalsForChecksum)
{
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
    const std::optional<TrustedBookEvent> event = adapter.process(R"({
      "channel":"book","type":"snapshot","data":[{
        "symbol":"UNI/USD",
        "asks":[{"price":4.5100,"qty":0.0100}],
        "bids":[{"price":4.5000,"qty":2.500}],
        "checksum":744964701,
        "timestamp":"2026-09-02T12:00:00Z"
      }]
    })");

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, TrustedBookEventType::Snapshot);
    EXPECT_TRUE(event->book.has_value());
    EXPECT_EQ(adapter.checksum_input(), "45100100450002500");
}

TEST(KrakenBookAdapterTest, KeepsDistinctExactPricesThatCollideAsDouble)
{
    ASSERT_EQ(
        std::stod("1.00000000000000001"),
        std::stod("1.00000000000000002")
    );
    KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
    const std::optional<TrustedBookEvent> event = adapter.process(R"({
      "channel":"book","type":"snapshot","data":[{
        "symbol":"UNI/USD",
        "asks":[
          {"price":"1.00000000000000002","qty":"2"},
          {"price":"1.00000000000000001","qty":"1"}
        ],
        "bids":[{"price":"0.9","qty":"3"}],
        "checksum":3418149461,
        "timestamp":"2026-09-02T12:00:00Z"
      }]
    })");

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, TrustedBookEventType::Snapshot);
    EXPECT_EQ(adapter.checksum(), 3418149461U);
    EXPECT_EQ(
        adapter.checksum_input(),
        "1000000000000000011100000000000000002293"
    );

    const std::optional<TrustedBookEvent> update = adapter.process(R"({
      "channel":"book","type":"update","data":[{
        "symbol":"UNI/USD",
        "asks":[{"price":"1.00000000000000001","qty":"0"}],
        "bids":[],
        "checksum":686055065,
        "timestamp":"2026-09-02T12:00:00.1Z"
      }]
    })");

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->type, TrustedBookEventType::Update);
    EXPECT_EQ(adapter.checksum_input(), "100000000000000002293");
}

TEST(KrakenBookAdapterTest, RejectsIncompleteEmptyAndWrongSymbolSnapshots)
{
    constexpr std::array<std::string_view, 3> invalid_messages{
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"UNI/USD","asks":[],"bids":[],"checksum":0,"timestamp":"2026-09-02T12:00:00Z"}]})",
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"ATOM/USD","asks":[{"price":"4.51","qty":"1"}],"bids":[{"price":"4.50","qty":"1"}],"checksum":0,"timestamp":"2026-09-02T12:00:00Z"}]})",
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"UNI/USD","asks":[{"price":"4.51","qty":"1"}],"checksum":0,"timestamp":"2026-09-02T12:00:00Z"}]})"
    };

    for (const std::string_view message : invalid_messages)
    {
        KrakenBookAdapter adapter(Product("UNI", "USD"), "UNI/USD", 500);
        const std::optional<TrustedBookEvent> event = adapter.process(message);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(event->type, TrustedBookEventType::Invalidated);
        EXPECT_EQ(adapter.status(), VenueMarketStatus::Stale);
    }
}
