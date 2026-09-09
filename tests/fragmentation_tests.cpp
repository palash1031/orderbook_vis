#include "consolidated_quote.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <utility>

namespace
{
struct Level
{
    double price;
    double quantity;
};

OrderBook book(
    std::optional<Level> bid,
    std::optional<Level> ask)
{
    OrderBook result;

    if (bid)
    {
        result.apply_update(BookSide::Bid, bid->price, bid->quantity);
    }

    if (ask)
    {
        result.apply_update(BookSide::Offer, ask->price, ask->quantity);
    }

    return result;
}

VenueBookState live(
    Venue venue,
    const Product& product,
    std::optional<Level> bid,
    std::optional<Level> ask)
{
    return {
        {venue, product},
        VenueMarketStatus::Live,
        book(bid, ask),
        {}
    };
}

VenueBookState unavailable(
    Venue venue,
    const Product& product,
    VenueMarketStatus status)
{
    return {{venue, product}, status, std::nullopt, {}};
}

TrustedBookEvent snapshot(
    Venue venue,
    const Product& product,
    Level bid,
    Level ask)
{
    return {
        TrustedBookEventType::Snapshot,
        {venue, product},
        {},
        book(bid, ask)
    };
}
}

TEST(FragmentationTest, CalculatesDifferencesFromConsolidatedMidpoint)
{
    const Product product("UNI", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{99.0, 20.0},
                Level{102.0, 30.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{98.5, 15.0},
                Level{101.0, 25.0}
            )
        }
    );

    ASSERT_TRUE(quote.fragmentation.has_value());
    EXPECT_NEAR(quote.fragmentation->bid_difference_bps, 50.0, 1e-12);
    EXPECT_NEAR(quote.fragmentation->ask_difference_bps, 100.0, 1e-12);
    EXPECT_NEAR(quote.fragmentation->max_difference_bps, 100.0, 1e-12);
}

TEST(FragmentationTest, EqualVenueQuotesProduceZeroDifference)
{
    const Product product("BTC", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{100.0, 2.0},
                Level{102.0, 3.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{100.0, 4.0},
                Level{102.0, 5.0}
            )
        }
    );

    ASSERT_TRUE(quote.fragmentation.has_value());
    EXPECT_DOUBLE_EQ(quote.fragmentation->bid_difference_bps, 0.0);
    EXPECT_DOUBLE_EQ(quote.fragmentation->ask_difference_bps, 0.0);
    EXPECT_DOUBLE_EQ(quote.fragmentation->max_difference_bps, 0.0);
}

TEST(FragmentationTest, RequiresBothVenuesToBeLive)
{
    const Product product("ETH", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{2'000.0, 2.0},
                Level{2'001.0, 3.0}
            ),
            unavailable(
                Venue::Kraken,
                product,
                VenueMarketStatus::Stale
            )
        }
    );

    EXPECT_FALSE(quote.fragmentation.has_value());
}

TEST(FragmentationTest, RequiresBothBids)
{
    const Product product("DOT", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                std::nullopt,
                Level{5.1, 3.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{5.0, 2.0},
                Level{5.2, 4.0}
            )
        }
    );

    EXPECT_FALSE(quote.fragmentation.has_value());
}

TEST(FragmentationTest, RequiresBothAsks)
{
    const Product product("BAT", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{0.25, 500.0},
                Level{0.26, 600.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{0.24, 400.0},
                std::nullopt
            )
        }
    );

    EXPECT_FALSE(quote.fragmentation.has_value());
}

TEST(FragmentationTest, InvalidConsolidatedMidpointProducesNoMetrics)
{
    const Product product("CAKE", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{-2.0, 1.0},
                Level{1.0, 1.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{-1.0, 1.0},
                Level{2.0, 1.0}
            )
        }
    );

    ASSERT_TRUE(quote.best_bid.has_value());
    ASSERT_TRUE(quote.best_ask.has_value());
    EXPECT_DOUBLE_EQ(
        (quote.best_bid->price + quote.best_ask->price) / 2.0,
        0.0
    );
    EXPECT_FALSE(quote.fragmentation.has_value());
}

TEST(FragmentationTest, VenueInvalidationRemovesMetricsButKeepsLiveQuote)
{
    const Product product("HBAR", "USD");
    const MarketKey kraken{Venue::Kraken, product};
    MarketStateStore store;
    store.apply(snapshot(
        Venue::Coinbase,
        product,
        Level{0.10, 1'000.0},
        Level{0.11, 2'000.0}
    ));
    store.apply(snapshot(
        Venue::Kraken,
        product,
        Level{0.101, 900.0},
        Level{0.112, 1'800.0}
    ));

    ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        store.product_states(product)
    );
    ASSERT_TRUE(quote.fragmentation.has_value());

    store.apply({
        TrustedBookEventType::Invalidated,
        kraken,
        {},
        std::nullopt
    });
    quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        store.product_states(product)
    );

    EXPECT_FALSE(quote.fragmentation.has_value());
    ASSERT_TRUE(quote.best_bid.has_value());
    EXPECT_EQ(quote.best_bid->venue, Venue::Coinbase);
    ASSERT_TRUE(quote.best_ask.has_value());
    EXPECT_EQ(quote.best_ask->venue, Venue::Coinbase);
}
