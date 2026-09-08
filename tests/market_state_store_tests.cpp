#include "market_state_store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

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

OrderBook consistent_book(double marker)
{
    return book(
        marker,
        marker + 100'000.0,
        marker + 1.0,
        marker + 100'001.0
    );
}

MarketTimestamp timestamp(std::int64_t nanoseconds)
{
    return MarketTimestamp{std::chrono::nanoseconds{nanoseconds}};
}

void expect_consistent_book(const VenueBookState& state)
{
    ASSERT_TRUE(state.book.has_value());
    const std::optional<double> bid = state.book->best_bid();
    const std::optional<double> ask = state.book->best_ask();
    ASSERT_TRUE(bid.has_value());
    ASSERT_TRUE(ask.has_value());
    EXPECT_DOUBLE_EQ(*ask, *bid + 1.0);
    EXPECT_DOUBLE_EQ(state.book->bids().at(*bid), *bid + 100'000.0);
    EXPECT_DOUBLE_EQ(state.book->asks().at(*ask), *bid + 100'001.0);
}
}

TEST(MarketStateStoreTest, SnapshotStoresLiveCompleteBook)
{
    MarketStateStore store;
    const MarketKey market{Venue::Coinbase, Product{"UNI", "USD"}};
    const MarketTimestamp exchange_timestamp = timestamp(42);

    store.apply({
        TrustedBookEventType::Snapshot,
        market,
        exchange_timestamp,
        book(4.57, 200.0, 4.58, 310.0)
    });

    const VenueBookState state = store.get(market).value();
    EXPECT_EQ(state.market, market);
    EXPECT_EQ(state.status, VenueMarketStatus::Live);
    EXPECT_EQ(state.exchange_timestamp, exchange_timestamp);
    ASSERT_TRUE(state.book.has_value());
    EXPECT_EQ(state.book->best_bid(), 4.57);
    EXPECT_EQ(state.book->best_ask(), 4.58);
    EXPECT_DOUBLE_EQ(state.book->bids().at(4.57), 200.0);
    EXPECT_DOUBLE_EQ(state.book->asks().at(4.58), 310.0);
}

TEST(MarketStateStoreTest, UpdateReplacesPreviouslyLiveCompleteBook)
{
    MarketStateStore store;
    const MarketKey market{Venue::Kraken, Product{"UNI", "USD"}};
    store.apply({
        TrustedBookEventType::Snapshot,
        market,
        timestamp(1),
        book(4.50, 10.0, 4.60, 20.0)
    });

    store.apply({
        TrustedBookEventType::Update,
        market,
        timestamp(2),
        book(4.57, 200.0, 4.58, 310.0)
    });

    const VenueBookState state = store.get(market).value();
    EXPECT_EQ(state.status, VenueMarketStatus::Live);
    EXPECT_EQ(state.exchange_timestamp, timestamp(2));
    ASSERT_TRUE(state.book.has_value());
    EXPECT_EQ(state.book->bid_levels(), 1U);
    EXPECT_EQ(state.book->ask_levels(), 1U);
    EXPECT_EQ(state.book->best_bid(), 4.57);
    EXPECT_EQ(state.book->best_ask(), 4.58);
}

TEST(MarketStateStoreTest, UpdateBeforeSnapshotIsRejected)
{
    MarketStateStore store;
    const MarketKey market{Venue::Kraken, Product{"UNI", "USD"}};
    store.set_status(market, VenueMarketStatus::WaitingForSnapshot);

    store.apply({
        TrustedBookEventType::Update,
        market,
        timestamp(2),
        book(4.57, 200.0, 4.58, 310.0)
    });

    const VenueBookState state = store.get(market).value();
    EXPECT_EQ(state.status, VenueMarketStatus::WaitingForSnapshot);
    EXPECT_FALSE(state.book.has_value());
    EXPECT_EQ(state.exchange_timestamp, MarketTimestamp{});
}

TEST(MarketStateStoreTest, InvalidationMakesBookStaleAndIneligible)
{
    MarketStateStore store;
    const MarketKey market{Venue::Kraken, Product{"UNI", "USD"}};
    store.apply({
        TrustedBookEventType::Snapshot,
        market,
        timestamp(1),
        book(4.57, 200.0, 4.58, 310.0)
    });

    store.apply({
        TrustedBookEventType::Invalidated,
        market,
        timestamp(3),
        std::nullopt
    });

    const VenueBookState state = store.get(market).value();
    EXPECT_EQ(state.status, VenueMarketStatus::Stale);
    EXPECT_FALSE(state.book.has_value());
    EXPECT_EQ(state.exchange_timestamp, timestamp(3));
}

TEST(MarketStateStoreTest, EveryNonLiveStatusClearsStoredBook)
{
    MarketStateStore store;
    const MarketKey market{Venue::Coinbase, Product{"BTC", "USD"}};
    const std::array statuses{
        VenueMarketStatus::Unsupported,
        VenueMarketStatus::Connecting,
        VenueMarketStatus::WaitingForSnapshot,
        VenueMarketStatus::Stale,
        VenueMarketStatus::Disconnected,
        VenueMarketStatus::Reconnecting
    };

    for (const VenueMarketStatus status : statuses)
    {
        SCOPED_TRACE(venue_market_status_name(status));
        store.apply({
            TrustedBookEventType::Snapshot,
            market,
            timestamp(9),
            book(100.0, 2.0, 101.0, 3.0)
        });

        store.set_status(market, status);

        const VenueBookState state = store.get(market).value();
        EXPECT_EQ(state.status, status);
        EXPECT_FALSE(state.book.has_value());
        EXPECT_EQ(state.exchange_timestamp, timestamp(9));
    }
}

TEST(MarketStateStoreTest, LiveStatusCannotBeManufacturedWithoutTrustedBook)
{
    MarketStateStore store;
    const MarketKey market{Venue::Coinbase, Product{"BTC", "USD"}};

    EXPECT_THROW(
        store.set_status(market, VenueMarketStatus::Live),
        std::invalid_argument
    );
    EXPECT_FALSE(store.get(market).has_value());
}

TEST(MarketStateStoreTest, TrustedBookEventsRequireBookState)
{
    MarketStateStore store;
    const MarketKey market{Venue::Coinbase, Product{"BTC", "USD"}};

    EXPECT_THROW(
        store.apply({
            TrustedBookEventType::Snapshot,
            market,
            timestamp(1),
            std::nullopt
        }),
        std::invalid_argument
    );
    EXPECT_THROW(
        store.apply({
            TrustedBookEventType::Update,
            market,
            timestamp(2),
            std::nullopt
        }),
        std::invalid_argument
    );
}

TEST(MarketStateStoreTest, ProductStatesAreFilteredAndVenueOrdered)
{
    MarketStateStore store;
    const Product uni{"UNI", "USD"};
    const MarketKey coinbase_uni{Venue::Coinbase, uni};
    const MarketKey kraken_uni{Venue::Kraken, uni};
    const MarketKey coinbase_btc{Venue::Coinbase, Product{"BTC", "USD"}};
    store.set_status(kraken_uni, VenueMarketStatus::Unsupported);
    store.apply({
        TrustedBookEventType::Snapshot,
        coinbase_btc,
        timestamp(1),
        book(100.0, 1.0, 101.0, 1.0)
    });
    store.apply({
        TrustedBookEventType::Snapshot,
        coinbase_uni,
        timestamp(2),
        book(4.57, 2.0, 4.58, 3.0)
    });

    const std::vector<VenueBookState> states = store.product_states(uni);

    ASSERT_EQ(states.size(), 2U);
    EXPECT_EQ(states[0].market, coinbase_uni);
    EXPECT_EQ(states[0].status, VenueMarketStatus::Live);
    EXPECT_TRUE(states[0].book.has_value());
    EXPECT_EQ(states[1].market, kraken_uni);
    EXPECT_EQ(states[1].status, VenueMarketStatus::Unsupported);
    EXPECT_FALSE(states[1].book.has_value());
}

TEST(MarketStateStoreTest, StaleKrakenDoesNotAlterLiveCoinbase)
{
    MarketStateStore store;
    const Product uni{"UNI", "USD"};
    const MarketKey coinbase{Venue::Coinbase, uni};
    const MarketKey kraken{Venue::Kraken, uni};
    store.apply({
        TrustedBookEventType::Snapshot,
        coinbase,
        timestamp(1),
        book(4.57, 2.0, 4.58, 3.0)
    });
    store.apply({
        TrustedBookEventType::Snapshot,
        kraken,
        timestamp(1),
        book(4.56, 4.0, 4.59, 5.0)
    });

    store.apply({
        TrustedBookEventType::Invalidated,
        kraken,
        timestamp(2),
        std::nullopt
    });

    const VenueBookState coinbase_state = store.get(coinbase).value();
    const VenueBookState kraken_state = store.get(kraken).value();
    EXPECT_EQ(coinbase_state.status, VenueMarketStatus::Live);
    EXPECT_TRUE(coinbase_state.book.has_value());
    EXPECT_EQ(kraken_state.status, VenueMarketStatus::Stale);
    EXPECT_FALSE(kraken_state.book.has_value());
}

TEST(MarketStateStoreTest, StatusNamesAreStableScannerValues)
{
    EXPECT_EQ(
        venue_market_status_name(VenueMarketStatus::Unsupported),
        "unsupported"
    );
    EXPECT_EQ(
        venue_market_status_name(VenueMarketStatus::Connecting),
        "connecting"
    );
    EXPECT_EQ(
        venue_market_status_name(VenueMarketStatus::WaitingForSnapshot),
        "waiting_for_snapshot"
    );
    EXPECT_EQ(venue_market_status_name(VenueMarketStatus::Live), "live");
    EXPECT_EQ(venue_market_status_name(VenueMarketStatus::Stale), "stale");
    EXPECT_EQ(
        venue_market_status_name(VenueMarketStatus::Disconnected),
        "disconnected"
    );
    EXPECT_EQ(
        venue_market_status_name(VenueMarketStatus::Reconnecting),
        "reconnecting"
    );
    EXPECT_THROW(
        venue_market_status_name(static_cast<VenueMarketStatus>(999)),
        std::invalid_argument
    );
}

TEST(MarketStateStoreTest, ConcurrentReplacementNeverExposesTornBook)
{
    MarketStateStore store;
    const MarketKey market{Venue::Coinbase, Product{"SOL", "USD"}};
    store.apply({
        TrustedBookEventType::Snapshot,
        market,
        timestamp(1),
        consistent_book(1.0)
    });

    std::atomic<bool> start{false};
    std::atomic<int> writers_remaining{2};
    std::atomic<bool> consistent{true};

    const auto writer = [&](double offset)
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        for (int index = 0; index < 2'000; ++index)
        {
            const double marker = offset + static_cast<double>(index);
            store.apply({
                TrustedBookEventType::Update,
                market,
                timestamp(index + 2),
                consistent_book(marker)
            });
        }

        writers_remaining.fetch_sub(1, std::memory_order_release);
    };

    std::thread first_writer(writer, 1'000.0);
    std::thread second_writer(writer, 10'000.0);
    std::thread reader([&]
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        while (writers_remaining.load(std::memory_order_acquire) > 0)
        {
            const VenueBookState state = store.get(market).value();
            if (!state.book)
            {
                consistent.store(false, std::memory_order_release);
                continue;
            }

            const std::optional<double> bid = state.book->best_bid();
            const std::optional<double> ask = state.book->best_ask();
            if (
                !bid
                || !ask
                || *ask != *bid + 1.0
                || state.book->bids().at(*bid) != *bid + 100'000.0
                || state.book->asks().at(*ask) != *bid + 100'001.0
            )
            {
                consistent.store(false, std::memory_order_release);
            }
        }
    });

    start.store(true, std::memory_order_release);
    first_writer.join();
    second_writer.join();
    reader.join();

    EXPECT_TRUE(consistent.load(std::memory_order_acquire));
    expect_consistent_book(store.get(market).value());
}

TEST(MarketStateStoreTest, ConcurrentMarketsRetainIndependentFinalStates)
{
    MarketStateStore store;
    const MarketKey coinbase{Venue::Coinbase, Product{"BTC", "USD"}};
    const MarketKey kraken{Venue::Kraken, Product{"UNI", "USD"}};
    store.apply({
        TrustedBookEventType::Snapshot,
        coinbase,
        timestamp(1),
        consistent_book(1.0)
    });
    store.apply({
        TrustedBookEventType::Snapshot,
        kraken,
        timestamp(1),
        consistent_book(2.0)
    });

    const auto update_market = [&](const MarketKey& market, double offset)
    {
        for (int index = 0; index < 1'000; ++index)
        {
            store.apply({
                TrustedBookEventType::Update,
                market,
                timestamp(index + 2),
                consistent_book(offset + static_cast<double>(index))
            });
        }
    };

    std::thread coinbase_writer(update_market, coinbase, 1'000.0);
    std::thread kraken_writer(update_market, kraken, 10'000.0);
    coinbase_writer.join();
    kraken_writer.join();

    const VenueBookState coinbase_state = store.get(coinbase).value();
    const VenueBookState kraken_state = store.get(kraken).value();
    expect_consistent_book(coinbase_state);
    expect_consistent_book(kraken_state);
    EXPECT_EQ(coinbase_state.book->best_bid(), 1'999.0);
    EXPECT_EQ(kraken_state.book->best_bid(), 10'999.0);
}
