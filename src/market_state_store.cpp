#include "market_state_store.hpp"

#include <mutex>
#include <stdexcept>

namespace
{
void validate_non_live_status(VenueMarketStatus status)
{
    switch (status)
    {
        case VenueMarketStatus::Unsupported:
        case VenueMarketStatus::Connecting:
        case VenueMarketStatus::WaitingForSnapshot:
        case VenueMarketStatus::Stale:
        case VenueMarketStatus::Disconnected:
        case VenueMarketStatus::Reconnecting:
            return;
        case VenueMarketStatus::Live:
            throw std::invalid_argument(
                "Live market status requires a trusted book event"
            );
    }

    throw std::invalid_argument("Unsupported venue market status");
}
}

void MarketStateStore::apply(const TrustedBookEvent& event)
{
    if (
        event.type != TrustedBookEventType::Invalidated
        && !event.book.has_value()
    )
    {
        throw std::invalid_argument("Trusted book event requires book state");
    }

    std::unique_lock lock(mutex_);

    switch (event.type)
    {
        case TrustedBookEventType::Snapshot:
            states_.insert_or_assign(
                event.market,
                VenueBookState{
                    event.market,
                    VenueMarketStatus::Live,
                    event.book,
                    event.timestamp
                }
            );
            return;

        case TrustedBookEventType::Update:
        {
            const auto state = states_.find(event.market);

            if (
                state == states_.end()
                || state->second.status != VenueMarketStatus::Live
            )
            {
                return;
            }

            state->second.book = event.book;
            state->second.exchange_timestamp = event.timestamp;
            return;
        }

        case TrustedBookEventType::Invalidated:
            states_.insert_or_assign(
                event.market,
                VenueBookState{
                    event.market,
                    VenueMarketStatus::Stale,
                    std::nullopt,
                    event.timestamp
                }
            );
            return;
    }

    throw std::invalid_argument("Unsupported trusted book event type");
}

void MarketStateStore::set_status(
    const MarketKey& market,
    VenueMarketStatus status)
{
    validate_non_live_status(status);
    std::unique_lock lock(mutex_);
    const auto state = states_.find(market);

    if (state == states_.end())
    {
        states_.emplace(
            market,
            VenueBookState{market, status, std::nullopt, {}}
        );
        return;
    }

    state->second.status = status;
    state->second.book.reset();
}

std::optional<VenueBookState> MarketStateStore::get(
    const MarketKey& market) const
{
    std::shared_lock lock(mutex_);
    const auto state = states_.find(market);

    if (state == states_.end())
    {
        return std::nullopt;
    }

    return state->second;
}

std::vector<VenueBookState> MarketStateStore::product_states(
    const Product& product) const
{
    std::shared_lock lock(mutex_);
    std::vector<VenueBookState> result;

    for (const auto& [market, state] : states_)
    {
        if (market.product == product)
        {
            result.push_back(state);
        }
    }

    return result;
}
