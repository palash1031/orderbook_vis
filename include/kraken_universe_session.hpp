#pragma once

#include "kraken_adapter.hpp"
#include "kraken_level2_stream.hpp"
#include "live_source.hpp"
#include "universe.hpp"
#include "venue_session.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

class KrakenUniverseSession final : public VenueUniverseSession
{
public:
    KrakenUniverseSession(
        MarketUniverse universe,
        std::unique_ptr<KrakenWire> wire,
        std::size_t depth = default_kraken_book_depth
    );

    Venue venue() const noexcept override;
    VenueSessionEvent read_event() override;

private:
    void initialize();
    void configure(const KrakenInstrumentCatalog& catalog);
    void process_acknowledgement(const std::string& raw_message);
    VenueSessionEvent pop_pending();

    MarketUniverse universe_;
    std::unique_ptr<KrakenWire> wire_;
    std::size_t depth_;
    std::map<std::string, Product> product_by_native_;
    std::map<Product, KrakenBookAdapter> adapters_;
    std::set<Product> supported_;
    std::set<Product> unsupported_;
    std::set<Product> acknowledged_;
    std::deque<VenueSessionEvent> pending_;
    bool reconnect_required_ = false;
};

class KrakenUniverseRunner
{
public:
    KrakenUniverseRunner(
        MarketUniverse universe,
        VenueSessionEventSink sink,
        VenueUniverseSessionFactory factory,
        ReconnectBackoffConfig backoff = {},
        LiveSourceSleeper sleeper = {}
    );
    KrakenUniverseRunner(
        MarketUniverse universe,
        VenueSessionEventSink sink,
        KrakenWireFactory factory,
        std::size_t depth = default_kraken_book_depth,
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
    KrakenWireFactory wire_factory_;
    std::size_t depth_ = default_kraken_book_depth;
    LiveSourceSleeper sleeper_;
    ReconnectBackoff backoff_;
    std::set<Product> unsupported_;
};
