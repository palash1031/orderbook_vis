#include "order_book.hpp"

#include <stdexcept>

void OrderBook::clear()
{
    bids_.clear();
    asks_.clear();
}

void OrderBook::apply_update(
    BookSide side,
    Price price,
    Quantity quantity)
{
    std::map<Price, Quantity>* levels = nullptr;

    switch (side)
    {
        case BookSide::Bid:
            levels = &bids_;
            break;

        case BookSide::Offer:
            levels = &asks_;
            break;

        default:
            throw std::invalid_argument("Unknown order book side");
    }

    if (quantity == 0.0)
    {
        levels->erase(price);
    }
    else
    {
        (*levels)[price] = quantity;
    }
}

std::size_t OrderBook::bid_levels() const
{
    return bids_.size();
}

std::size_t OrderBook::ask_levels() const
{
    return asks_.size();
}

std::optional<OrderBook::Price> OrderBook::best_bid() const
{
    if (bids_.empty())
    {
        return std::nullopt;
    }

    return bids_.rbegin()->first;
}

std::optional<OrderBook::Price> OrderBook::best_ask() const
{
    if (asks_.empty())
    {
        return std::nullopt;
    }

    return asks_.begin()->first;
}

std::optional<OrderBook::Price> OrderBook::spread() const
{
    const auto bid = best_bid();
    const auto ask = best_ask();

    if (!bid || !ask)
    {
        return std::nullopt;
    }

    return *ask - *bid;
}

const OrderBook::Levels& OrderBook::bids() const noexcept
{
    return bids_;
}

const OrderBook::Levels& OrderBook::asks() const noexcept
{
    return asks_;
}
