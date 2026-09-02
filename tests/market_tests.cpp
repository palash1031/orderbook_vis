#include "market.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <unordered_set>

TEST(MarketTest, ProductNormalizesAndFormatsCanonicalIdentity)
{
    const Product product{" uni ", "usd"};

    EXPECT_EQ(product.base(), "UNI");
    EXPECT_EQ(product.quote(), "USD");
    EXPECT_EQ(product.to_string(), "UNI-USD");
    EXPECT_EQ(Product::parse(" atom-usdc "), Product("ATOM", "USDC"));
}

TEST(MarketTest, ProductRejectsMalformedComponentsAndPairs)
{
    EXPECT_THROW(Product("", "USD"), std::invalid_argument);
    EXPECT_THROW(Product("UNI/USD", "USD"), std::invalid_argument);
    EXPECT_THROW(Product("UNI", "US D"), std::invalid_argument);
    EXPECT_THROW(Product::parse("UNI/USD"), std::invalid_argument);
    EXPECT_THROW(Product::parse("UNI--USD"), std::invalid_argument);
}

TEST(MarketTest, VenueAndMarketKeyHaveStableValueSemantics)
{
    const MarketKey coinbase{Venue::Coinbase, Product("UNI", "USD")};
    const MarketKey kraken{Venue::Kraken, Product("UNI", "USD")};
    const MarketKey kraken_atom{Venue::Kraken, Product("ATOM", "USD")};

    EXPECT_EQ(venue_name(Venue::Coinbase), "coinbase");
    EXPECT_EQ(venue_name(Venue::Kraken), "kraken");
    EXPECT_EQ(parse_venue(" KRAKEN "), Venue::Kraken);
    EXPECT_THROW(parse_venue("gemini"), std::invalid_argument);
    EXPECT_LT(coinbase, kraken);
    EXPECT_LT(kraken_atom, kraken);

    const std::unordered_set<MarketKey> keys{coinbase, kraken, kraken};
    EXPECT_EQ(keys.size(), 2U);
}
