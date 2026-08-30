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
    using Levels = std::map<Price, Quantity>;

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

    const Levels& bids() const noexcept;
    const Levels& asks() const noexcept;

private:
    Levels bids_;
    Levels asks_;
};
