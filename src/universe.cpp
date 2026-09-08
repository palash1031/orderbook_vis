#include "universe.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

MarketUniverse::MarketUniverse(std::vector<Product> products)
    : products_(std::move(products))
{
    if (products_.empty())
    {
        throw std::invalid_argument("Market universe cannot be empty");
    }

    std::set<Product> unique_products;

    for (const Product& product : products_)
    {
        if (!unique_products.insert(product).second)
        {
            throw std::invalid_argument(
                "Duplicate market universe product: " + product.to_string()
            );
        }
    }
}

MarketUniverse MarketUniverse::default_usd()
{
    return MarketUniverse({
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
    });
}

const std::vector<Product>& MarketUniverse::products() const noexcept
{
    return products_;
}

bool MarketUniverse::contains(const Product& product) const noexcept
{
    return std::find(products_.begin(), products_.end(), product)
        != products_.end();
}

std::size_t MarketUniverse::size() const noexcept
{
    return products_.size();
}
