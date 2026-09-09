#include "consolidated_quote.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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

VenueBookState state(
    Venue venue,
    const Product& product,
    VenueMarketStatus status,
    std::optional<OrderBook> order_book = std::nullopt)
{
    return {{venue, product}, status, std::move(order_book), {}};
}

VenueBookState live(
    Venue venue,
    const Product& product,
    std::optional<Level> bid,
    std::optional<Level> ask)
{
    return state(
        venue,
        product,
        VenueMarketStatus::Live,
        book(bid, ask)
    );
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

void expect_quote(
    const std::optional<VenueQuote>& actual,
    Venue venue,
    double price,
    double quantity)
{
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->venue, venue);
    EXPECT_DOUBLE_EQ(actual->price, price);
    EXPECT_DOUBLE_EQ(actual->quantity, quantity);
}
}

TEST(ConsolidatedQuoteEngineTest, SelectsBestPricesAndRetainsVenueTopOfBook)
{
    const Product product("UNI", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{4.572, 200.0},
                Level{4.578, 310.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{4.574, 150.0},
                Level{4.581, 270.0}
            )
        }
    );

    EXPECT_EQ(quote.product, product);
    expect_quote(quote.best_bid, Venue::Kraken, 4.574, 150.0);
    expect_quote(quote.best_ask, Venue::Coinbase, 4.578, 310.0);

    ASSERT_EQ(quote.venues.size(), 2U);
    const VenueTopOfBook& coinbase = quote.venues.at(Venue::Coinbase);
    EXPECT_EQ(coinbase.status, VenueMarketStatus::Live);
    expect_quote(coinbase.bid, Venue::Coinbase, 4.572, 200.0);
    expect_quote(coinbase.ask, Venue::Coinbase, 4.578, 310.0);

    const VenueTopOfBook& kraken = quote.venues.at(Venue::Kraken);
    EXPECT_EQ(kraken.status, VenueMarketStatus::Live);
    expect_quote(kraken.bid, Venue::Kraken, 4.574, 150.0);
    expect_quote(kraken.ask, Venue::Kraken, 4.581, 270.0);
}

TEST(ConsolidatedQuoteEngineTest, StaleKrakenCannotBeatLiveCoinbase)
{
    const Product product("BTC", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{100.0, 2.0},
                Level{101.0, 3.0}
            ),
            state(
                Venue::Kraken,
                product,
                VenueMarketStatus::Stale,
                book(Level{110.0, 50.0}, Level{90.0, 60.0})
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Coinbase, 100.0, 2.0);
    expect_quote(quote.best_ask, Venue::Coinbase, 101.0, 3.0);
    EXPECT_FALSE(quote.venues.at(Venue::Kraken).bid.has_value());
    EXPECT_FALSE(quote.venues.at(Venue::Kraken).ask.has_value());
}

TEST(ConsolidatedQuoteEngineTest, ReconnectingCoinbaseLeavesKrakenUsable)
{
    const Product product("ETH", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            state(
                Venue::Coinbase,
                product,
                VenueMarketStatus::Reconnecting
            ),
            live(
                Venue::Kraken,
                product,
                Level{2'000.0, 5.0},
                Level{2'001.0, 6.0}
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Kraken, 2'000.0, 5.0);
    expect_quote(quote.best_ask, Venue::Kraken, 2'001.0, 6.0);
}

TEST(ConsolidatedQuoteEngineTest, NeitherLiveVenueProducesNoBestQuote)
{
    const Product product("HBAR", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            state(
                Venue::Coinbase,
                product,
                VenueMarketStatus::WaitingForSnapshot
            ),
            state(
                Venue::Kraken,
                product,
                VenueMarketStatus::Unsupported
            )
        }
    );

    EXPECT_FALSE(quote.best_bid.has_value());
    EXPECT_FALSE(quote.best_ask.has_value());
    EXPECT_EQ(
        quote.venues.at(Venue::Coinbase).status,
        VenueMarketStatus::WaitingForSnapshot
    );
    EXPECT_EQ(
        quote.venues.at(Venue::Kraken).status,
        VenueMarketStatus::Unsupported
    );
}

TEST(ConsolidatedQuoteEngineTest, LargerQuantityBreaksEqualPriceTie)
{
    const Product product("SOL", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{100.0, 4.0},
                Level{101.0, 9.0}
            ),
            live(
                Venue::Kraken,
                product,
                Level{100.0, 5.0},
                Level{101.0, 8.0}
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Kraken, 100.0, 5.0);
    expect_quote(quote.best_ask, Venue::Coinbase, 101.0, 9.0);
}

TEST(ConsolidatedQuoteEngineTest, VenueOrderingBreaksExactTie)
{
    const Product product("ATOM", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Kraken,
                product,
                Level{10.0, 7.0},
                Level{11.0, 8.0}
            ),
            live(
                Venue::Coinbase,
                product,
                Level{10.0, 7.0},
                Level{11.0, 8.0}
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Coinbase, 10.0, 7.0);
    expect_quote(quote.best_ask, Venue::Coinbase, 11.0, 8.0);
}

TEST(ConsolidatedQuoteEngineTest, MissingBidDoesNotHideAvailableAsk)
{
    const Product product("DOT", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                std::nullopt,
                Level{5.0, 3.0}
            ),
            state(
                Venue::Kraken,
                product,
                VenueMarketStatus::Unsupported
            )
        }
    );

    EXPECT_FALSE(quote.best_bid.has_value());
    expect_quote(quote.best_ask, Venue::Coinbase, 5.0, 3.0);
}

TEST(ConsolidatedQuoteEngineTest, MissingAskDoesNotHideAvailableBid)
{
    const Product product("BAT", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            state(
                Venue::Coinbase,
                product,
                VenueMarketStatus::Connecting
            ),
            live(
                Venue::Kraken,
                product,
                Level{0.25, 500.0},
                std::nullopt
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Kraken, 0.25, 500.0);
    EXPECT_FALSE(quote.best_ask.has_value());
}

TEST(ConsolidatedQuoteEngineTest, UnsupportedVenueNeverContributesQuotes)
{
    const Product product("CAKE", "USD");
    const ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        {
            live(
                Venue::Coinbase,
                product,
                Level{2.0, 1.0},
                Level{2.1, 1.0}
            ),
            state(
                Venue::Kraken,
                product,
                VenueMarketStatus::Unsupported,
                book(Level{20.0, 1.0}, Level{0.2, 1.0})
            )
        }
    );

    expect_quote(quote.best_bid, Venue::Coinbase, 2.0, 1.0);
    expect_quote(quote.best_ask, Venue::Coinbase, 2.1, 1.0);
}

TEST(ConsolidatedQuoteEngineTest, StoreInvalidationRemovesFormerBestVenue)
{
    const Product product("UNI", "USD");
    const MarketKey kraken{Venue::Kraken, product};
    MarketStateStore store;
    store.apply(snapshot(
        Venue::Coinbase,
        product,
        Level{4.57, 200.0},
        Level{4.58, 310.0}
    ));
    store.apply(snapshot(
        Venue::Kraken,
        product,
        Level{4.575, 150.0},
        Level{4.59, 270.0}
    ));

    ConsolidatedQuote quote = ConsolidatedQuoteEngine{}.calculate(
        product,
        store.product_states(product)
    );
    expect_quote(quote.best_bid, Venue::Kraken, 4.575, 150.0);

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

    expect_quote(quote.best_bid, Venue::Coinbase, 4.57, 200.0);
    EXPECT_EQ(
        quote.venues.at(Venue::Kraken).status,
        VenueMarketStatus::Stale
    );
    EXPECT_FALSE(quote.venues.at(Venue::Kraken).bid.has_value());
}

TEST(ConsolidatedQuoteEngineTest, RejectsStateForAnotherProduct)
{
    const Product uni("UNI", "USD");
    const Product btc("BTC", "USD");

    EXPECT_THROW(
        ConsolidatedQuoteEngine{}.calculate(
            uni,
            {
                live(
                    Venue::Coinbase,
                    btc,
                    Level{100.0, 1.0},
                    Level{101.0, 1.0}
                )
            }
        ),
        std::invalid_argument
    );
}

TEST(ConsolidatedQuoteEngineTest, RejectsDuplicateVenueStates)
{
    const Product product("UNI", "USD");

    EXPECT_THROW(
        ConsolidatedQuoteEngine{}.calculate(
            product,
            {
                live(
                    Venue::Coinbase,
                    product,
                    Level{4.57, 1.0},
                    Level{4.58, 1.0}
                ),
                live(
                    Venue::Coinbase,
                    product,
                    Level{4.56, 2.0},
                    Level{4.59, 2.0}
                )
            }
        ),
        std::invalid_argument
    );
}
