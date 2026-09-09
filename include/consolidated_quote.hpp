#pragma once

#include "market_state_store.hpp"

#include <map>
#include <optional>
#include <vector>

struct VenueQuote
{
    Venue venue;
    double price;
    double quantity;
};

struct VenueTopOfBook
{
    VenueMarketStatus status;
    std::optional<VenueQuote> bid;
    std::optional<VenueQuote> ask;
};

struct ConsolidatedQuote
{
    Product product;
    std::map<Venue, VenueTopOfBook> venues;
    std::optional<VenueQuote> best_bid;
    std::optional<VenueQuote> best_ask;
};

class ConsolidatedQuoteEngine
{
public:
    ConsolidatedQuote calculate(
        const Product& product,
        const std::vector<VenueBookState>& states
    ) const;
};
