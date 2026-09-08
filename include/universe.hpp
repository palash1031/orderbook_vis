#pragma once

#include "market.hpp"

#include <cstddef>
#include <vector>

class MarketUniverse
{
public:
    explicit MarketUniverse(std::vector<Product> products);

    static MarketUniverse default_usd();

    const std::vector<Product>& products() const noexcept;
    bool contains(const Product& product) const noexcept;
    std::size_t size() const noexcept;

private:
    std::vector<Product> products_;
};
