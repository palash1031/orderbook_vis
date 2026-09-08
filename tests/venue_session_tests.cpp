#include "venue_session.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace
{
class ScriptedVenueSession final : public VenueUniverseSession
{
public:
    explicit ScriptedVenueSession(VenueSessionEvent event)
        : event_(std::move(event))
    {
    }

    Venue venue() const noexcept override
    {
        return Venue::Coinbase;
    }

    VenueSessionEvent read_event() override
    {
        return event_;
    }

private:
    VenueSessionEvent event_;
};
}

TEST(VenueSessionTest, StatusEventCarriesCanonicalMarketIdentity)
{
    const MarketKey market{Venue::Kraken, Product{" uni ", " usd "}};
    const VenueSessionEvent event = VenueMarketStatusEvent{
        market,
        VenueMarketStatus::WaitingForSnapshot
    };

    ASSERT_TRUE(std::holds_alternative<VenueMarketStatusEvent>(event));
    const auto& status = std::get<VenueMarketStatusEvent>(event);
    EXPECT_EQ(status.market, market);
    EXPECT_EQ(status.market.product.to_string(), "UNI-USD");
    EXPECT_EQ(status.status, VenueMarketStatus::WaitingForSnapshot);
}

TEST(VenueSessionTest, VariantCarriesTrustedBookEventWithoutTranslation)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 4.57, 200.0);
    const MarketKey market{Venue::Coinbase, Product{"UNI", "USD"}};
    const VenueSessionEvent event = TrustedBookEvent{
        TrustedBookEventType::Snapshot,
        market,
        {},
        std::move(book)
    };

    ASSERT_TRUE(std::holds_alternative<TrustedBookEvent>(event));
    const auto& trusted = std::get<TrustedBookEvent>(event);
    EXPECT_EQ(trusted.market, market);
    ASSERT_TRUE(trusted.book.has_value());
    EXPECT_EQ(trusted.book->best_bid(), 4.57);
}

TEST(VenueSessionTest, SinkAndFactoryUseTheCommonSessionInterface)
{
    static_assert(std::is_same_v<
        VenueUniverseSessionFactory::result_type,
        std::unique_ptr<VenueUniverseSession>
    >);

    const MarketKey market{Venue::Coinbase, Product{"BTC", "USD"}};
    VenueUniverseSessionFactory factory = [market]
    {
        return std::make_unique<ScriptedVenueSession>(
            VenueMarketStatusEvent{market, VenueMarketStatus::Connecting}
        );
    };
    std::optional<VenueMarketStatusEvent> received;
    const VenueSessionEventSink sink = [&received](const VenueSessionEvent& event)
    {
        if (const auto* status = std::get_if<VenueMarketStatusEvent>(&event))
        {
            received = *status;
        }
    };

    std::unique_ptr<VenueUniverseSession> session = factory();
    EXPECT_EQ(session->venue(), Venue::Coinbase);
    sink(session->read_event());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->market, market);
    EXPECT_EQ(received->status, VenueMarketStatus::Connecting);
}
