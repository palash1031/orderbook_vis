#include "universe.hpp"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <vector>

namespace
{
const std::vector<Product> expected_default_products{
    Product{"CAKE", "USD"},
    Product{"DASH", "USD"},
    Product{"ATOM", "USD"},
    Product{"POL", "USD"},
    Product{"ICP", "USD"},
    Product{"DOT", "USD"},
    Product{"HBAR", "USD"},
    Product{"UNI", "USD"},
    Product{"BAT", "USD"},
    Product{"ETH", "USD"},
    Product{"BTC", "USD"},
    Product{"SOL", "USD"}
};
}

TEST(MarketUniverseTest, DefaultUsdHasConfiguredCanonicalOrder)
{
    const MarketUniverse universe = MarketUniverse::default_usd();

    EXPECT_EQ(universe.size(), 12U);
    EXPECT_EQ(universe.products(), expected_default_products);
}

TEST(MarketUniverseTest, DefaultUsdContainsEveryProductWithoutDuplicates)
{
    const MarketUniverse universe = MarketUniverse::default_usd();
    const std::set<Product> unique(
        universe.products().begin(),
        universe.products().end()
    );

    EXPECT_EQ(unique.size(), universe.size());

    for (const Product& product : expected_default_products)
    {
        EXPECT_TRUE(universe.contains(product)) << product.to_string();
    }
}

TEST(MarketUniverseTest, CustomUniversePreservesOrderAndSupportsLookup)
{
    const std::vector<Product> configured{
        Product{"sol", "usd"},
        Product{" uni ", " usd "},
        Product{"BTC", "USD"}
    };
    const MarketUniverse universe(configured);

    EXPECT_EQ(universe.products(), configured);
    EXPECT_EQ(universe.size(), 3U);
    EXPECT_TRUE(universe.contains(Product{"UNI", "USD"}));
    EXPECT_FALSE(universe.contains(Product{"ETH", "USD"}));
}

TEST(MarketUniverseTest, RejectsEmptyUniverse)
{
    EXPECT_THROW(MarketUniverse({}), std::invalid_argument);
}

TEST(MarketUniverseTest, RejectsDuplicatesAfterProductNormalization)
{
    EXPECT_THROW(
        MarketUniverse({
            Product{"uni", "usd"},
            Product{" UNI ", " USD "}
        }),
        std::invalid_argument
    );
}

TEST(MarketUniverseTest, ProductValidationRemainsAuthoritative)
{
    EXPECT_THROW(
        []
        {
            const MarketUniverse universe({Product{"UNI/USD", "USD"}});
            static_cast<void>(universe);
        }(),
        std::invalid_argument
    );
}
