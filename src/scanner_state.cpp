#include "scanner_state.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace
{
constexpr Venue scanner_venues[]{Venue::Coinbase, Venue::Kraken};

MarketKey event_market(const VenueSessionEvent& event)
{
    return std::visit(
        [](const auto& value)
        {
            return value.market;
        },
        event
    );
}
}

ScannerStatePublisher::ScannerStatePublisher(
    MarketUniverse universe,
    std::shared_ptr<MarketStateStore> store,
    std::shared_ptr<ScannerStreamHub> hub)
    : universe_(std::move(universe)),
      store_(std::move(store)),
      hub_(std::move(hub))
{
    if (!store_ || !hub_)
    {
        throw std::invalid_argument(
            "Scanner state publisher requires a store and stream hub"
        );
    }

    for (const Product& product : universe_.products())
    {
        for (const Venue venue : scanner_venues)
        {
            store_->set_status(
                {venue, product},
                VenueMarketStatus::Connecting
            );
        }
    }
}

void ScannerStatePublisher::apply(const VenueSessionEvent& event)
{
    const MarketKey market = event_market(event);

    if (!universe_.contains(market.product))
    {
        throw std::invalid_argument(
            "Scanner event product is outside the configured universe"
        );
    }

    std::lock_guard lock(mutex_);
    std::visit(
        [this](const auto& value)
        {
            using Value = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<Value, TrustedBookEvent>)
            {
                store_->apply(value);
            }
            else
            {
                store_->set_status(value.market, value.status);
            }
        },
        event
    );

    hub_->publish(quote_engine_.calculate(
        market.product,
        store_->product_states(market.product)
    ));
}
