#include "scanner_stream.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
struct Level
{
    double price;
    double quantity;
};

OrderBook book(Level bid, Level ask)
{
    OrderBook result;
    result.apply_update(BookSide::Bid, bid.price, bid.quantity);
    result.apply_update(BookSide::Offer, ask.price, ask.quantity);
    return result;
}

VenueBookState live(
    Venue venue,
    const Product& product,
    Level bid,
    Level ask)
{
    return {
        {venue, product},
        VenueMarketStatus::Live,
        book(bid, ask),
        {}
    };
}

ConsolidatedQuote quote(
    const Product& product,
    Level coinbase_bid,
    Level coinbase_ask,
    Level kraken_bid,
    Level kraken_ask)
{
    return ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                coinbase_bid,
                coinbase_ask
            ),
            live(
                Venue::Kraken,
                product,
                kraken_bid,
                kraken_ask
            )
        }
    );
}

json::object next_message(
    const std::shared_ptr<ScannerStreamSubscriber>& client)
{
    const std::optional<std::string> message = client->wait_for_message(
        std::chrono::milliseconds{0}
    );
    EXPECT_TRUE(message.has_value());
    return json::parse(*message).as_object();
}

void drain_initial(
    const std::shared_ptr<ScannerStreamSubscriber>& client,
    std::size_t product_count)
{
    for (std::size_t index = 0; index < product_count + 1; ++index)
    {
        next_message(client);
    }
}
}

TEST(ScannerStreamHubTest, NewClientReceivesHelloThenEveryConfiguredProduct)
{
    const MarketUniverse universe({
        Product{"BTC", "USD"},
        Product{"UNI", "USD"},
        Product{"HBAR", "USD"}
    });
    ScannerStreamHub hub(universe);
    const auto client = hub.subscribe();

    const json::object hello = next_message(client);
    EXPECT_EQ(hello.at("type").as_string(), "scanner_hello");
    EXPECT_EQ(hello.at("schema_version").as_int64(), 1);
    ASSERT_EQ(hello.at("venues").as_array().size(), 2U);
    EXPECT_EQ(hello.at("venues").as_array()[0].as_string(), "coinbase");
    EXPECT_EQ(hello.at("venues").as_array()[1].as_string(), "kraken");

    const json::array& products = hello.at("products").as_array();
    ASSERT_EQ(products.size(), 3U);
    EXPECT_EQ(products[0].as_string(), "BTC-USD");
    EXPECT_EQ(products[1].as_string(), "UNI-USD");
    EXPECT_EQ(products[2].as_string(), "HBAR-USD");

    for (const std::string_view product_id : {
             std::string_view{"BTC-USD"},
             std::string_view{"UNI-USD"},
             std::string_view{"HBAR-USD"}
         })
    {
        const json::object update = next_message(client);
        EXPECT_EQ(update.at("type").as_string(), "scanner_update");
        EXPECT_EQ(update.at("product_id").as_string(), product_id);
        const json::object& venues = update.at("venues").as_object();
        EXPECT_EQ(
            venues.at("coinbase").as_object().at("status").as_string(),
            "connecting"
        );
        EXPECT_EQ(
            venues.at("kraken").as_object().at("status").as_string(),
            "connecting"
        );
        EXPECT_TRUE(
            venues.at("coinbase").as_object().at("bid").is_null()
        );
        EXPECT_TRUE(update.at("fragmentation").is_null());
    }

    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(ScannerStreamHubTest, PublishesCompleteIncrementalProductUpdate)
{
    const Product uni("UNI", "USD");
    ScannerStreamHub hub(MarketUniverse({uni}));
    const auto client = hub.subscribe();
    drain_initial(client, 1);

    hub.publish(quote(
        uni,
        Level{99.0, 20.0},
        Level{102.0, 30.0},
        Level{98.5, 15.0},
        Level{101.0, 25.0}
    ));

    const json::object update = next_message(client);
    EXPECT_EQ(update.at("type").as_string(), "scanner_update");
    EXPECT_EQ(update.at("product_id").as_string(), "UNI-USD");

    const json::object& venues = update.at("venues").as_object();
    const json::object& coinbase = venues.at("coinbase").as_object();
    EXPECT_EQ(coinbase.at("status").as_string(), "live");
    EXPECT_DOUBLE_EQ(json::value_to<double>(coinbase.at("bid")), 99.0);
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(coinbase.at("bid_quantity")),
        20.0
    );
    EXPECT_DOUBLE_EQ(json::value_to<double>(coinbase.at("ask")), 102.0);
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(coinbase.at("ask_quantity")),
        30.0
    );

    const json::object& consolidated =
        update.at("consolidated").as_object();
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(consolidated.at("best_bid")),
        99.0
    );
    EXPECT_EQ(
        consolidated.at("best_bid_venue").as_string(),
        "coinbase"
    );
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(consolidated.at("best_ask")),
        101.0
    );
    EXPECT_EQ(
        consolidated.at("best_ask_venue").as_string(),
        "kraken"
    );

    const json::object& fragmentation =
        update.at("fragmentation").as_object();
    EXPECT_NEAR(
        json::value_to<double>(fragmentation.at("bid_bps")),
        50.0,
        1e-12
    );
    EXPECT_NEAR(
        json::value_to<double>(fragmentation.at("ask_bps")),
        100.0,
        1e-12
    );
    EXPECT_NEAR(
        json::value_to<double>(fragmentation.at("max_bps")),
        100.0,
        1e-12
    );
}

TEST(ScannerStreamHubTest, SendsOnlyChangedProductToConnectedClient)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    ScannerStreamHub hub(MarketUniverse({btc, uni}));
    const auto client = hub.subscribe();
    drain_initial(client, 2);

    hub.publish(quote(
        uni,
        Level{4.57, 20.0},
        Level{4.58, 30.0},
        Level{4.56, 15.0},
        Level{4.59, 25.0}
    ));

    EXPECT_EQ(next_message(client).at("product_id").as_string(), "UNI-USD");
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(ScannerStreamHubTest, LateClientReceivesLatestStateInUniverseOrder)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    ScannerStreamHub hub(MarketUniverse({btc, uni}));
    hub.publish(quote(
        uni,
        Level{4.57, 20.0},
        Level{4.58, 30.0},
        Level{4.56, 15.0},
        Level{4.59, 25.0}
    ));
    hub.publish(quote(
        btc,
        Level{100.0, 2.0},
        Level{102.0, 3.0},
        Level{101.0, 4.0},
        Level{103.0, 5.0}
    ));

    const auto client = hub.subscribe();
    EXPECT_EQ(next_message(client).at("type").as_string(), "scanner_hello");
    const json::object btc_update = next_message(client);
    EXPECT_EQ(btc_update.at("product_id").as_string(), "BTC-USD");
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(
            btc_update.at("consolidated").as_object().at("best_bid")
        ),
        101.0
    );
    const json::object uni_update = next_message(client);
    EXPECT_EQ(uni_update.at("product_id").as_string(), "UNI-USD");
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(
            uni_update.at("consolidated").as_object().at("best_ask")
        ),
        4.58
    );
}

TEST(ScannerStreamHubTest, SuppressesDuplicateTopOfBookUpdate)
{
    const Product product("SOL", "USD");
    ScannerStreamHub hub(MarketUniverse({product}));
    const auto client = hub.subscribe();
    drain_initial(client, 1);
    const ConsolidatedQuote current = quote(
        product,
        Level{100.0, 2.0},
        Level{102.0, 3.0},
        Level{101.0, 4.0},
        Level{103.0, 5.0}
    );

    hub.publish(current);
    next_message(client);
    hub.publish(current);

    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(ScannerStreamHubTest, RejectsUnknownProductAndIncompleteVenueState)
{
    const Product btc("BTC", "USD");
    const Product doge("DOGE", "USD");
    ScannerStreamHub hub(MarketUniverse({btc}));

    EXPECT_THROW(
        hub.publish(quote(
            doge,
            Level{1.0, 1.0},
            Level{2.0, 1.0},
            Level{1.0, 1.0},
            Level{2.0, 1.0}
        )),
        std::invalid_argument
    );

    const ConsolidatedQuote incomplete =
        ConsolidatedQuoteEngine{}.calculate(
            btc,
            {
                live(
                    Venue::Coinbase,
                    btc,
                    Level{100.0, 1.0},
                    Level{101.0, 1.0}
                )
            }
        );
    EXPECT_THROW(hub.publish(incomplete), std::invalid_argument);
}
