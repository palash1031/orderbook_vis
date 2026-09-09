#include "scanner_state.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace json = boost::json;

namespace
{
OrderBook book(
    double bid,
    double bid_quantity,
    double ask,
    double ask_quantity)
{
    OrderBook result;
    result.apply_update(BookSide::Bid, bid, bid_quantity);
    result.apply_update(BookSide::Offer, ask, ask_quantity);
    return result;
}

TrustedBookEvent trusted(
    Venue venue,
    const Product& product,
    TrustedBookEventType type,
    double bid,
    double bid_quantity,
    double ask,
    double ask_quantity)
{
    return {
        type,
        {venue, product},
        {},
        book(bid, bid_quantity, ask, ask_quantity)
    };
}

TrustedBookEvent invalidated(Venue venue, const Product& product)
{
    return {
        TrustedBookEventType::Invalidated,
        {venue, product},
        {},
        std::nullopt
    };
}

VenueMarketStatusEvent status(
    Venue venue,
    const Product& product,
    VenueMarketStatus value)
{
    return {{venue, product}, value};
}

json::object next_update(
    const std::shared_ptr<ScannerStreamSubscriber>& client)
{
    const std::optional<std::string> message = client->wait_for_message(
        std::chrono::milliseconds{0}
    );

    if (!message)
    {
        throw std::runtime_error("Expected scanner message");
    }

    return json::parse(*message).as_object();
}

void drain_initial(
    const std::shared_ptr<ScannerStreamSubscriber>& client,
    std::size_t product_count)
{
    for (std::size_t index = 0; index < product_count + 1; ++index)
    {
        next_update(client);
    }
}

std::string_view best_venue(
    const json::object& update,
    std::string_view side)
{
    return update.at("consolidated")
        .as_object()
        .at(std::string("best_") + std::string(side) + "_venue")
        .as_string();
}

std::string_view venue_status(
    const json::object& update,
    std::string_view venue)
{
    return update.at("venues")
        .as_object()
        .at(venue)
        .as_object()
        .at("status")
        .as_string();
}
}

TEST(ScannerAcceptanceTest, ThreeProductsConsolidateInterleavedVenueEvents)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const Product hbar("HBAR", "USD");
    const MarketUniverse universe({btc, uni, hbar});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher scanner(universe, store, hub);
    const auto client = hub->subscribe();
    drain_initial(client, universe.size());

    scanner.apply(trusted(
        Venue::Coinbase, btc, TrustedBookEventType::Snapshot,
        100.0, 5.0, 104.0, 6.0
    ));
    next_update(client);
    scanner.apply(trusted(
        Venue::Kraken, btc, TrustedBookEventType::Snapshot,
        102.0, 7.0, 105.0, 8.0
    ));
    json::object update = next_update(client);
    EXPECT_EQ(best_venue(update, "bid"), "kraken");
    EXPECT_EQ(best_venue(update, "ask"), "coinbase");
    EXPECT_NEAR(
        json::value_to<double>(
            update.at("fragmentation").as_object().at("max_bps")
        ),
        2.0 / 103.0 * 10'000.0,
        1e-9
    );

    scanner.apply(trusted(
        Venue::Coinbase, uni, TrustedBookEventType::Snapshot,
        10.0, 9.0, 11.0, 10.0
    ));
    next_update(client);
    scanner.apply(status(
        Venue::Kraken, uni, VenueMarketStatus::Unsupported
    ));
    update = next_update(client);
    EXPECT_EQ(venue_status(update, "kraken"), "unsupported");
    EXPECT_EQ(best_venue(update, "bid"), "coinbase");
    EXPECT_TRUE(update.at("fragmentation").is_null());

    scanner.apply(trusted(
        Venue::Kraken, hbar, TrustedBookEventType::Snapshot,
        0.100, 11.0, 0.110, 12.0
    ));
    next_update(client);
    scanner.apply(trusted(
        Venue::Coinbase, hbar, TrustedBookEventType::Snapshot,
        0.101, 13.0, 0.112, 14.0
    ));
    update = next_update(client);
    EXPECT_EQ(best_venue(update, "bid"), "coinbase");
    EXPECT_EQ(best_venue(update, "ask"), "kraken");

    scanner.apply(trusted(
        Venue::Coinbase, btc, TrustedBookEventType::Update,
        103.0, 15.0, 106.0, 16.0
    ));
    update = next_update(client);
    EXPECT_EQ(update.at("product_id").as_string(), "BTC-USD");
    EXPECT_EQ(best_venue(update, "bid"), "coinbase");
    EXPECT_EQ(best_venue(update, "ask"), "kraken");
}

TEST(ScannerAcceptanceTest, FailuresAreVenueLocalAndRecoveryNeedsSnapshots)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const Product hbar("HBAR", "USD");
    const MarketUniverse universe({btc, uni, hbar});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher scanner(universe, store, hub);
    const auto client = hub->subscribe();
    drain_initial(client, universe.size());

    for (const Product& product : universe.products())
    {
        scanner.apply(trusted(
            Venue::Coinbase, product, TrustedBookEventType::Snapshot,
            100.0, 2.0, 103.0, 3.0
        ));
        next_update(client);
    }

    for (const Product& product : {btc, uni})
    {
        scanner.apply(trusted(
            Venue::Kraken, product, TrustedBookEventType::Snapshot,
            101.0, 4.0, 102.0, 5.0
        ));
        next_update(client);
    }
    scanner.apply(status(
        Venue::Kraken, hbar, VenueMarketStatus::Unsupported
    ));
    next_update(client);

    scanner.apply(invalidated(Venue::Kraken, btc));
    json::object update = next_update(client);
    EXPECT_EQ(venue_status(update, "kraken"), "stale");
    EXPECT_EQ(best_venue(update, "bid"), "coinbase");
    EXPECT_TRUE(update.at("fragmentation").is_null());

    for (const VenueMarketStatus recovery_status : {
             VenueMarketStatus::Disconnected,
             VenueMarketStatus::Reconnecting,
             VenueMarketStatus::WaitingForSnapshot
         })
    {
        scanner.apply(status(Venue::Kraken, uni, recovery_status));
        update = next_update(client);
        EXPECT_EQ(best_venue(update, "bid"), "coinbase");
        EXPECT_TRUE(update.at("fragmentation").is_null());
    }

    scanner.apply(trusted(
        Venue::Kraken, uni, TrustedBookEventType::Update,
        999.0, 99.0, 1'000.0, 99.0
    ));
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));

    scanner.apply(trusted(
        Venue::Kraken, uni, TrustedBookEventType::Snapshot,
        101.0, 7.0, 102.0, 8.0
    ));
    update = next_update(client);
    EXPECT_EQ(venue_status(update, "kraken"), "live");
    EXPECT_EQ(best_venue(update, "bid"), "kraken");
    EXPECT_TRUE(update.at("fragmentation").is_object());

    for (const Product& product : universe.products())
    {
        scanner.apply(invalidated(Venue::Coinbase, product));
        update = next_update(client);
        EXPECT_EQ(venue_status(update, "coinbase"), "stale");

        if (product == uni)
        {
            EXPECT_EQ(best_venue(update, "bid"), "kraken");
        }
    }

    for (const Product& product : universe.products())
    {
        scanner.apply(status(
            Venue::Coinbase,
            product,
            VenueMarketStatus::WaitingForSnapshot
        ));
        next_update(client);
    }

    scanner.apply(trusted(
        Venue::Coinbase, btc, TrustedBookEventType::Snapshot,
        104.0, 10.0, 105.0, 11.0
    ));
    update = next_update(client);
    EXPECT_EQ(venue_status(update, "coinbase"), "live");

    const auto late_client = hub->subscribe();
    EXPECT_EQ(next_update(late_client).at("type").as_string(), "scanner_hello");
    const json::object late_btc = next_update(late_client);
    const json::object late_uni = next_update(late_client);
    const json::object late_hbar = next_update(late_client);
    EXPECT_EQ(late_btc.at("product_id").as_string(), "BTC-USD");
    EXPECT_EQ(venue_status(late_btc, "coinbase"), "live");
    EXPECT_EQ(late_uni.at("product_id").as_string(), "UNI-USD");
    EXPECT_EQ(venue_status(late_uni, "coinbase"), "waiting_for_snapshot");
    EXPECT_EQ(venue_status(late_uni, "kraken"), "live");
    EXPECT_EQ(late_hbar.at("product_id").as_string(), "HBAR-USD");
    EXPECT_EQ(venue_status(late_hbar, "kraken"), "unsupported");
    EXPECT_EQ(
        store->get({Venue::Coinbase, uni})->status,
        VenueMarketStatus::WaitingForSnapshot
    );
    EXPECT_EQ(
        store->get({Venue::Kraken, uni})->status,
        VenueMarketStatus::Live
    );
}
