#pragma once

#include "book_reconstructor.hpp"
#include "coinbase_wire.hpp"
#include "live_source.hpp"
#include "sequence_tracker.hpp"
#include "universe.hpp"
#include "venue_session.hpp"

#include <deque>
#include <map>
#include <memory>
#include <set>

enum class CoinbaseSubscriptionDisposition
{
    Accepted,
    PermanentProductRejection,
    SessionFailure
};

class CoinbaseUniverseSession final : public VenueUniverseSession
{
public:
    CoinbaseUniverseSession(
        MarketUniverse universe,
        std::unique_ptr<CoinbaseWire> wire
    );

    Venue venue() const noexcept override;
    VenueSessionEvent read_event() override;

private:
    VenueSessionEvent pop_pending();
    void process_frame(std::string_view raw_message);
    void invalidate_all();

    MarketUniverse universe_;
    std::unique_ptr<CoinbaseWire> wire_;
    SequenceTracker sequence_;
    std::map<Product, BookReconstructor> books_;
    std::set<Product> unsupported_;
    std::deque<VenueSessionEvent> pending_;
    bool reconnect_required_ = false;
};

class CoinbaseUniverseRunner
{
public:
    CoinbaseUniverseRunner(
        MarketUniverse universe,
        VenueSessionEventSink sink,
        VenueUniverseSessionFactory factory,
        ReconnectBackoffConfig backoff = {},
        LiveSourceSleeper sleeper = {}
    );
    CoinbaseUniverseRunner(
        MarketUniverse universe,
        VenueSessionEventSink sink,
        CoinbaseWireFactory factory,
        ReconnectBackoffConfig backoff = {},
        LiveSourceSleeper sleeper = {}
    );

    void run(LiveSourceStopCheck should_stop = {});

private:
    bool valid_market(const MarketKey& market) const noexcept;
    void publish_recovery_status(VenueMarketStatus status);
    std::unique_ptr<VenueUniverseSession> make_session() const;

    MarketUniverse universe_;
    VenueSessionEventSink sink_;
    VenueUniverseSessionFactory factory_;
    CoinbaseWireFactory wire_factory_;
    LiveSourceSleeper sleeper_;
    ReconnectBackoff backoff_;
    std::set<Product> unsupported_;
};
