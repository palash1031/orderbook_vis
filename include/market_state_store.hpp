#pragma once

#include "venue_adapter.hpp"

#include <map>
#include <optional>
#include <shared_mutex>
#include <vector>

struct VenueBookState
{
    MarketKey market;
    VenueMarketStatus status;
    std::optional<OrderBook> book;
    MarketTimestamp exchange_timestamp{};
};

class MarketStateStore
{
public:
    void apply(const TrustedBookEvent& event);
    void set_status(
        const MarketKey& market,
        VenueMarketStatus status
    );

    std::optional<VenueBookState> get(const MarketKey& market) const;
    std::vector<VenueBookState> product_states(
        const Product& product
    ) const;

private:
    mutable std::shared_mutex mutex_;
    std::map<MarketKey, VenueBookState> states_;
};
