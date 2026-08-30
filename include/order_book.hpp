#pragma once

#include "book_types.hpp"

#include <cstddef>
#include <map>
#include <optional>

class OrderBook
{
public:
    using Price = double;
    using Quantity = double;

    void clear();

    void apply_update(
        BookSide side,
        Price price,
        Quantity quantity
    );

    std::size_t bid_levels() const;
    std::size_t ask_levels() const;

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;

private:
    std::map<Price, Quantity> bids_;
    std::map<Price, Quantity> asks_;
};
