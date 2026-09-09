#include "consolidated_quote.hpp"

#include <stdexcept>

namespace
{
bool is_better_bid(
    const VenueQuote& candidate,
    const std::optional<VenueQuote>& current)
{
    if (!current)
    {
        return true;
    }

    if (candidate.price != current->price)
    {
        return candidate.price > current->price;
    }

    if (candidate.quantity != current->quantity)
    {
        return candidate.quantity > current->quantity;
    }

    return candidate.venue < current->venue;
}

bool is_better_ask(
    const VenueQuote& candidate,
    const std::optional<VenueQuote>& current)
{
    if (!current)
    {
        return true;
    }

    if (candidate.price != current->price)
    {
        return candidate.price < current->price;
    }

    if (candidate.quantity != current->quantity)
    {
        return candidate.quantity > current->quantity;
    }

    return candidate.venue < current->venue;
}

std::optional<VenueQuote> best_bid(
    Venue venue,
    const OrderBook& book)
{
    const std::optional<double> price = book.best_bid();

    if (!price)
    {
        return std::nullopt;
    }

    return VenueQuote{venue, *price, book.bids().at(*price)};
}

std::optional<VenueQuote> best_ask(
    Venue venue,
    const OrderBook& book)
{
    const std::optional<double> price = book.best_ask();

    if (!price)
    {
        return std::nullopt;
    }

    return VenueQuote{venue, *price, book.asks().at(*price)};
}
}

ConsolidatedQuote ConsolidatedQuoteEngine::calculate(
    const Product& product,
    const std::vector<VenueBookState>& states) const
{
    ConsolidatedQuote result{product, {}, std::nullopt, std::nullopt};

    for (const VenueBookState& state : states)
    {
        if (state.market.product != product)
        {
            throw std::invalid_argument(
                "Venue book state belongs to a different product"
            );
        }

        const auto [entry, inserted] = result.venues.emplace(
            state.market.venue,
            VenueTopOfBook{state.status, std::nullopt, std::nullopt}
        );

        if (!inserted)
        {
            throw std::invalid_argument(
                "Duplicate venue state for consolidated product"
            );
        }

        if (
            state.status != VenueMarketStatus::Live
            || !state.book.has_value()
        )
        {
            continue;
        }

        entry->second.bid = best_bid(state.market.venue, *state.book);
        entry->second.ask = best_ask(state.market.venue, *state.book);

        if (
            entry->second.bid
            && is_better_bid(*entry->second.bid, result.best_bid)
        )
        {
            result.best_bid = entry->second.bid;
        }

        if (
            entry->second.ask
            && is_better_ask(*entry->second.ask, result.best_ask)
        )
        {
            result.best_ask = entry->second.ask;
        }
    }

    return result;
}
