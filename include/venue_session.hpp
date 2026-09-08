#pragma once

#include "venue_adapter.hpp"

#include <functional>
#include <memory>
#include <variant>

struct VenueMarketStatusEvent
{
    MarketKey market;
    VenueMarketStatus status;
};

using VenueSessionEvent = std::variant<
    TrustedBookEvent,
    VenueMarketStatusEvent
>;

using VenueSessionEventSink = std::function<void(const VenueSessionEvent&)>;

class VenueUniverseSession
{
public:
    virtual ~VenueUniverseSession() = default;

    virtual Venue venue() const noexcept = 0;
    virtual VenueSessionEvent read_event() = 0;
};

using VenueUniverseSessionFactory =
    std::function<std::unique_ptr<VenueUniverseSession>()>;
