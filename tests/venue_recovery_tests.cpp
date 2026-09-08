#include "coinbase_universe_session.hpp"
#include "kraken_universe_session.hpp"
#include "market_state_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using namespace std::chrono_literals;

class StepGate
{
public:
    bool await(std::size_t target, bool advance)
    {
        std::unique_lock lock(mutex_);

        if (!changed_.wait_for(lock, 5s, [&]
            {
                return cancelled_ || step_ == target;
            }))
        {
            failed_ = true;
            cancelled_ = true;
            changed_.notify_all();
            return false;
        }

        if (cancelled_)
        {
            return false;
        }

        if (advance)
        {
            ++step_;
            changed_.notify_all();
        }

        return true;
    }

    bool advance(std::size_t target)
    {
        std::lock_guard lock(mutex_);

        if (cancelled_ || step_ != target)
        {
            failed_ = true;
            cancelled_ = true;
            changed_.notify_all();
            return false;
        }

        ++step_;
        changed_.notify_all();
        return true;
    }

    bool wait_until(std::size_t target)
    {
        std::unique_lock lock(mutex_);

        if (!changed_.wait_for(lock, 5s, [&]
            {
                return cancelled_ || step_ >= target;
            }))
        {
            failed_ = true;
            cancelled_ = true;
            changed_.notify_all();
            return false;
        }

        return !cancelled_;
    }

    void notify_all()
    {
        changed_.notify_all();
    }

    bool failed() const
    {
        std::lock_guard lock(mutex_);
        return failed_;
    }

    std::size_t step() const
    {
        std::lock_guard lock(mutex_);
        return step_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t step_ = 0;
    bool failed_ = false;
    bool cancelled_ = false;
};

struct ScriptAction
{
    std::size_t step;
    std::optional<VenueSessionEvent> event;
};

class CoordinatedVenueSession final : public VenueUniverseSession
{
public:
    CoordinatedVenueSession(
        Venue venue,
        StepGate& gate,
        std::deque<ScriptAction> actions)
        : venue_(venue),
          gate_(gate),
          actions_(std::move(actions))
    {
    }

    Venue venue() const noexcept override
    {
        return venue_;
    }

    VenueSessionEvent read_event() override
    {
        if (actions_.empty())
        {
            throw std::runtime_error("coordinated session exhausted");
        }

        ScriptAction action = std::move(actions_.front());
        actions_.pop_front();

        if (!gate_.await(action.step, action.event.has_value()))
        {
            throw std::runtime_error("coordinated session timed out");
        }

        if (!action.event)
        {
            throw std::runtime_error("scripted venue connection failed");
        }

        return std::move(*action.event);
    }

private:
    Venue venue_;
    StepGate& gate_;
    std::deque<ScriptAction> actions_;
};

TrustedBookEvent book_event(
    Venue venue,
    const Product& product,
    TrustedBookEventType type,
    double bid,
    double bid_quantity)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, bid, bid_quantity);
    book.apply_update(BookSide::Offer, bid + 1.0, 2.0);
    return {
        type,
        {venue, product},
        MarketTimestamp{std::chrono::nanoseconds{
            static_cast<std::int64_t>(bid_quantity)
        }},
        std::move(book)
    };
}

TrustedBookEvent invalidation(Venue venue, const Product& product)
{
    return {
        TrustedBookEventType::Invalidated,
        {venue, product},
        {},
        std::nullopt
    };
}

VenueMarketStatusEvent status_event(
    Venue venue,
    const Product& product,
    VenueMarketStatus status)
{
    return {{venue, product}, status};
}

VenueSessionEventSink store_sink(MarketStateStore& store)
{
    return [&](const VenueSessionEvent& event)
    {
        std::visit(
            [&](const auto& value)
            {
                using Value = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<Value, TrustedBookEvent>)
                {
                    store.apply(value);
                }
                else
                {
                    store.set_status(value.market, value.status);
                }
            },
            event
        );
    };
}

bool is_live(const MarketStateStore& store, const MarketKey& market)
{
    const std::optional<VenueBookState> state = store.get(market);
    return state
        && state->status == VenueMarketStatus::Live
        && state->book.has_value();
}

double bid_quantity(const MarketStateStore& store, const MarketKey& market)
{
    const VenueBookState state = store.get(market).value();
    const double price = state.book->best_bid().value();
    return state.book->bids().at(price);
}
}

TEST(VenueRecoveryTest, VenueFailuresAndRecoveryRemainIndependent)
{
    const Product btc("BTC", "USD");
    const Product uni("UNI", "USD");
    const MarketUniverse universe({btc, uni});
    const MarketKey coinbase_btc{Venue::Coinbase, btc};
    const MarketKey coinbase_uni{Venue::Coinbase, uni};
    const MarketKey kraken_btc{Venue::Kraken, btc};
    const MarketKey kraken_uni{Venue::Kraken, uni};
    MarketStateStore store;
    const VenueSessionEventSink apply = store_sink(store);
    StepGate gate;
    std::atomic<bool> stop = false;
    std::mutex observation_mutex;
    std::vector<std::string> failures;
    std::mutex stop_mutex;
    std::condition_variable stop_changed;

    const auto record = [&](bool condition, std::string message)
    {
        if (!condition)
        {
            std::lock_guard lock(observation_mutex);
            failures.push_back(std::move(message));
        }
    };

    std::size_t kraken_attempts = 0;
    CoinbaseUniverseRunner coinbase(
        universe,
        [&](const VenueSessionEvent& event)
        {
            apply(event);
        },
        VenueUniverseSessionFactory{
            [&]() -> std::unique_ptr<VenueUniverseSession>
            {
                return std::make_unique<CoordinatedVenueSession>(
                    Venue::Coinbase,
                    gate,
                    std::deque<ScriptAction>{
                        {0, book_event(Venue::Coinbase, btc, TrustedBookEventType::Snapshot, 100.0, 1.0)},
                        {1, book_event(Venue::Coinbase, uni, TrustedBookEventType::Snapshot, 10.0, 1.0)},
                        {5, book_event(Venue::Coinbase, btc, TrustedBookEventType::Update, 102.0, 9.0)},
                        {9, invalidation(Venue::Coinbase, btc)},
                        {10, invalidation(Venue::Coinbase, uni)},
                        {11, std::nullopt}
                    }
                );
            }
        },
        ReconnectBackoffConfig{1ms, 1ms, 0.0},
        [&](std::chrono::milliseconds)
        {
            record(!is_live(store, coinbase_btc), "Coinbase BTC stayed live after its gap");
            record(!is_live(store, coinbase_uni), "Coinbase UNI stayed live after its gap");
            record(is_live(store, kraken_btc), "Coinbase gap cleared live Kraken BTC");
            record(
                store.get(kraken_uni)->status
                    == VenueMarketStatus::WaitingForSnapshot,
                "Coinbase gap changed waiting Kraken UNI"
            );
            record(gate.advance(11), "Could not release Kraken after Coinbase failure");
            record(gate.wait_until(13), "Kraken did not continue after Coinbase failure");
            record(
                bid_quantity(store, kraken_btc) == 7.0,
                "Kraken update did not reach the store"
            );
            stop = true;
            stop_changed.notify_all();
            gate.notify_all();
        }
    );
    KrakenUniverseRunner kraken(
        universe,
        [&](const VenueSessionEvent& event)
        {
            apply(event);

            const auto* book = std::get_if<TrustedBookEvent>(&event);

            if (
                book
                && book->type == TrustedBookEventType::Update
                && book->market == kraken_btc
                && book->book->bids().at(102.0) == 7.0
            )
            {
                std::unique_lock lock(stop_mutex);
                stop_changed.wait_for(lock, 5s, [&]
                {
                    return stop.load();
                });
            }
        },
        VenueUniverseSessionFactory{
            [&]() -> std::unique_ptr<VenueUniverseSession>
            {
                ++kraken_attempts;

                if (kraken_attempts == 1)
                {
                    return std::make_unique<CoordinatedVenueSession>(
                        Venue::Kraken,
                        gate,
                        std::deque<ScriptAction>{
                            {2, book_event(Venue::Kraken, btc, TrustedBookEventType::Snapshot, 101.0, 1.0)},
                            {3, book_event(Venue::Kraken, uni, TrustedBookEventType::Snapshot, 11.0, 1.0)},
                            {4, std::nullopt}
                        }
                    );
                }

                return std::make_unique<CoordinatedVenueSession>(
                    Venue::Kraken,
                    gate,
                    std::deque<ScriptAction>{
                        {6, status_event(Venue::Kraken, btc, VenueMarketStatus::WaitingForSnapshot)},
                        {7, status_event(Venue::Kraken, uni, VenueMarketStatus::WaitingForSnapshot)},
                        {8, book_event(Venue::Kraken, btc, TrustedBookEventType::Snapshot, 102.0, 2.0)},
                        {12, book_event(Venue::Kraken, btc, TrustedBookEventType::Update, 102.0, 7.0)}
                    }
                );
            }
        },
        ReconnectBackoffConfig{1ms, 1ms, 0.0},
        [&](std::chrono::milliseconds)
        {
            record(is_live(store, coinbase_btc), "Kraken failure cleared Coinbase BTC");
            record(is_live(store, coinbase_uni), "Kraken failure cleared Coinbase UNI");
            record(!is_live(store, kraken_btc), "Failed Kraken BTC stayed eligible");
            record(!is_live(store, kraken_uni), "Failed Kraken UNI stayed eligible");
            record(gate.advance(4), "Could not release Coinbase after Kraken failure");
            record(gate.wait_until(6), "Coinbase did not update during Kraken recovery");
            record(
                bid_quantity(store, coinbase_btc) == 9.0,
                "Coinbase update stopped with Kraken"
            );
        }
    );

    std::thread coinbase_thread([&]
    {
        coinbase.run([&]
        {
            return stop.load() || gate.failed();
        });
    });
    std::thread kraken_thread([&]
    {
        kraken.run([&]
        {
            return stop.load() || gate.failed();
        });
    });

    coinbase_thread.join();
    kraken_thread.join();

    EXPECT_FALSE(gate.failed());
    EXPECT_EQ(gate.step(), 13U);
    EXPECT_EQ(kraken_attempts, 2U);
    EXPECT_TRUE(failures.empty()) << (
        failures.empty() ? "" : failures.front()
    );
    EXPECT_EQ(store.get(coinbase_btc)->status, VenueMarketStatus::Reconnecting);
    EXPECT_EQ(store.get(coinbase_uni)->status, VenueMarketStatus::Reconnecting);
    EXPECT_TRUE(is_live(store, kraken_btc));
    EXPECT_EQ(bid_quantity(store, kraken_btc), 7.0);
    EXPECT_EQ(
        store.get(kraken_uni)->status,
        VenueMarketStatus::WaitingForSnapshot
    );
}

TEST(VenueRecoveryTest, UnsupportedAndGenericFailuresAreVenueLocal)
{
    const Product hbar("HBAR", "USD");
    const Product btc("BTC", "USD");
    const Product eth("ETH", "USD");
    MarketStateStore store;
    const VenueSessionEventSink apply = store_sink(store);

    apply(book_event(
        Venue::Coinbase,
        hbar,
        TrustedBookEventType::Snapshot,
        10.0,
        1.0
    ));
    apply(status_event(
        Venue::Kraken,
        hbar,
        VenueMarketStatus::Unsupported
    ));
    EXPECT_TRUE(is_live(store, {Venue::Coinbase, hbar}));

    apply(book_event(
        Venue::Kraken,
        btc,
        TrustedBookEventType::Snapshot,
        100.0,
        1.0
    ));
    apply(status_event(
        Venue::Coinbase,
        btc,
        VenueMarketStatus::Unsupported
    ));
    EXPECT_TRUE(is_live(store, {Venue::Kraken, btc}));

    apply(book_event(
        Venue::Kraken,
        eth,
        TrustedBookEventType::Snapshot,
        200.0,
        1.0
    ));
    bool stop = false;
    CoinbaseUniverseRunner failing_coinbase(
        MarketUniverse({eth}),
        apply,
        VenueUniverseSessionFactory{
            []() -> std::unique_ptr<VenueUniverseSession>
            {
                throw std::runtime_error("generic socket failure");
            }
        },
        ReconnectBackoffConfig{1ms, 1ms, 0.0},
        [&](std::chrono::milliseconds)
        {
            stop = true;
        }
    );
    failing_coinbase.run([&stop]
    {
        return stop;
    });

    const VenueBookState coinbase_eth =
        store.get({Venue::Coinbase, eth}).value();
    EXPECT_EQ(coinbase_eth.status, VenueMarketStatus::Reconnecting);
    EXPECT_NE(coinbase_eth.status, VenueMarketStatus::Unsupported);
    EXPECT_TRUE(is_live(store, {Venue::Kraken, eth}));
}
