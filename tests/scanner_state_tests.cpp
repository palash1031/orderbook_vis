#include "scanner_state.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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

TrustedBookEvent book_event(
    Venue venue,
    const Product& product,
    TrustedBookEventType type,
    OrderBook order_book)
{
    return {
        type,
        {venue, product},
        {},
        std::move(order_book)
    };
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
    std::size_t products)
{
    for (std::size_t index = 0; index < products + 1; ++index)
    {
        next_message(client);
    }
}
}

TEST(ScannerStatePublisherTest, InitializesEveryMarketAsConnecting)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const MarketUniverse universe({btc, uni});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);

    ScannerStatePublisher publisher(universe, store, hub);

    for (const Product& product : universe.products())
    {
        for (const Venue venue : {Venue::Coinbase, Venue::Kraken})
        {
            const VenueBookState state = store->get({venue, product}).value();
            EXPECT_EQ(state.status, VenueMarketStatus::Connecting);
            EXPECT_FALSE(state.book.has_value());
        }
    }
}

TEST(ScannerStatePublisherTest, PublishesStatusAndTrustedBookForOneProduct)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const MarketUniverse universe({btc, uni});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher publisher(universe, store, hub);
    const auto client = hub->subscribe();
    drain_initial(client, 2);

    publisher.apply(VenueMarketStatusEvent{
        {Venue::Coinbase, btc},
        VenueMarketStatus::WaitingForSnapshot
    });
    json::object update = next_message(client);
    EXPECT_EQ(update.at("product_id").as_string(), "BTC-USD");
    EXPECT_EQ(
        update.at("venues")
            .as_object()
            .at("coinbase")
            .as_object()
            .at("status")
            .as_string(),
        "waiting_for_snapshot"
    );

    publisher.apply(book_event(
        Venue::Coinbase,
        btc,
        TrustedBookEventType::Snapshot,
        book(100.0, 2.0, 101.0, 3.0)
    ));
    update = next_message(client);
    EXPECT_EQ(update.at("product_id").as_string(), "BTC-USD");
    EXPECT_DOUBLE_EQ(
        json::value_to<double>(
            update.at("consolidated").as_object().at("best_bid")
        ),
        100.0
    );
    EXPECT_EQ(
        update.at("consolidated")
            .as_object()
            .at("best_bid_venue")
            .as_string(),
        "coinbase"
    );
    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(ScannerStatePublisherTest, InvalidationRemovesMetricsAndStaleVenue)
{
    const Product product("UNI", "USD");
    const MarketUniverse universe({product});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher publisher(universe, store, hub);
    const auto client = hub->subscribe();
    drain_initial(client, 1);

    publisher.apply(book_event(
        Venue::Coinbase,
        product,
        TrustedBookEventType::Snapshot,
        book(99.0, 20.0, 102.0, 30.0)
    ));
    next_message(client);
    publisher.apply(book_event(
        Venue::Kraken,
        product,
        TrustedBookEventType::Snapshot,
        book(98.5, 15.0, 101.0, 25.0)
    ));
    EXPECT_TRUE(next_message(client).at("fragmentation").is_object());

    publisher.apply(TrustedBookEvent{
        TrustedBookEventType::Invalidated,
        {Venue::Kraken, product},
        {},
        std::nullopt
    });

    const json::object update = next_message(client);
    EXPECT_EQ(
        update.at("venues")
            .as_object()
            .at("kraken")
            .as_object()
            .at("status")
            .as_string(),
        "stale"
    );
    EXPECT_TRUE(update.at("fragmentation").is_null());
    EXPECT_EQ(
        update.at("consolidated")
            .as_object()
            .at("best_bid_venue")
            .as_string(),
        "coinbase"
    );
}

TEST(ScannerStatePublisherTest, UnchangedTopOfBookDoesNotEmitUpdate)
{
    const Product product("SOL", "USD");
    const MarketUniverse universe({product});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher publisher(universe, store, hub);
    const auto client = hub->subscribe();
    drain_initial(client, 1);

    publisher.apply(book_event(
        Venue::Coinbase,
        product,
        TrustedBookEventType::Snapshot,
        book(100.0, 2.0, 101.0, 3.0)
    ));
    next_message(client);

    OrderBook deeper = book(100.0, 2.0, 101.0, 3.0);
    deeper.apply_update(BookSide::Bid, 99.0, 50.0);
    deeper.apply_update(BookSide::Offer, 102.0, 60.0);
    publisher.apply(book_event(
        Venue::Coinbase,
        product,
        TrustedBookEventType::Update,
        std::move(deeper)
    ));

    EXPECT_FALSE(client->wait_for_message(std::chrono::milliseconds{0}));
}

TEST(ScannerStatePublisherTest, RejectsEventOutsideConfiguredUniverse)
{
    const Product btc("BTC", "USD");
    const Product doge("DOGE", "USD");
    const MarketUniverse universe({btc});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);
    ScannerStatePublisher publisher(universe, store, hub);

    EXPECT_THROW(
        publisher.apply(VenueMarketStatusEvent{
            {Venue::Coinbase, doge},
            VenueMarketStatus::Unsupported
        }),
        std::invalid_argument
    );
    EXPECT_FALSE(store->get({Venue::Coinbase, doge}).has_value());
}

TEST(ScannerStatePublisherTest, RejectsMissingStoreOrHub)
{
    const MarketUniverse universe({Product{"BTC", "USD"}});
    auto store = std::make_shared<MarketStateStore>();
    auto hub = std::make_shared<ScannerStreamHub>(universe);

    EXPECT_THROW(
        ScannerStatePublisher(universe, nullptr, hub),
        std::invalid_argument
    );
    EXPECT_THROW(
        ScannerStatePublisher(universe, store, nullptr),
        std::invalid_argument
    );
}
