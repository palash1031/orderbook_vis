#pragma once

#include "market_state_store.hpp"
#include "scanner_stream.hpp"
#include "universe.hpp"
#include "venue_session.hpp"

#include <memory>
#include <mutex>

class ScannerStatePublisher
{
public:
    ScannerStatePublisher(
        MarketUniverse universe,
        std::shared_ptr<MarketStateStore> store,
        std::shared_ptr<ScannerStreamHub> hub
    );

    void apply(const VenueSessionEvent& event);

private:
    MarketUniverse universe_;
    std::shared_ptr<MarketStateStore> store_;
    std::shared_ptr<ScannerStreamHub> hub_;
    ConsolidatedQuoteEngine quote_engine_;
    std::mutex mutex_;
};
