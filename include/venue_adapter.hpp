#pragma once

#include "book_types.hpp"
#include "market.hpp"
#include "order_book.hpp"

#include <optional>
#include <string_view>

enum class VenueSessionStatus
{
    Connecting,
    Connected,
    Disconnected,
    Reconnecting
};

enum class VenueMarketStatus
{
    Unsupported,
    Connecting,
    WaitingForSnapshot,
    Live,
    Stale,
    Reconnecting
};

enum class TrustedBookEventType
{
    Snapshot,
    Update,
    Invalidated
};

struct TrustedBookEvent
{
    TrustedBookEventType type;
    MarketKey market;
    MarketTimestamp timestamp;
    std::optional<OrderBook> book;
};

class VenueAdapter
{
public:
    virtual ~VenueAdapter() = default;

    virtual std::optional<TrustedBookEvent> process(
        std::string_view raw_message
    ) = 0;
    virtual void reset() noexcept = 0;
    virtual VenueMarketStatus status() const noexcept = 0;
};
