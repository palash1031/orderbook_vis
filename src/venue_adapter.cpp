#include "venue_adapter.hpp"

#include <stdexcept>

std::string_view venue_market_status_name(VenueMarketStatus status)
{
    switch (status)
    {
        case VenueMarketStatus::Unsupported:
            return "unsupported";
        case VenueMarketStatus::Connecting:
            return "connecting";
        case VenueMarketStatus::WaitingForSnapshot:
            return "waiting_for_snapshot";
        case VenueMarketStatus::Live:
            return "live";
        case VenueMarketStatus::Stale:
            return "stale";
        case VenueMarketStatus::Disconnected:
            return "disconnected";
        case VenueMarketStatus::Reconnecting:
            return "reconnecting";
    }

    throw std::invalid_argument("Unsupported venue market status");
}
