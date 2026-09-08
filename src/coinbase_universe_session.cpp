#include "coinbase_universe_session.hpp"

#include "coinbase_parser.hpp"
#include "recorder_config.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
struct CoinbaseSubscriptionResult
{
    CoinbaseSubscriptionDisposition disposition;
    std::optional<Product> product;
    std::string detail;
};

std::string json_text(const json::value& value)
{
    const auto& text = value.as_string();
    return std::string(text.c_str(), text.size());
}

std::string lowercase(std::string_view text)
{
    std::string result;
    result.reserve(text.size());

    for (const char character : text)
    {
        if (character >= 'A' && character <= 'Z')
        {
            result.push_back(
                static_cast<char>(character - 'A' + 'a')
            );
        }
        else
        {
            result.push_back(character);
        }
    }

    return result;
}

bool permanent_product_rejection(std::string_view detail)
{
    const std::string normalized = lowercase(detail);
    return normalized.find("not supported") != std::string::npos
        || normalized.find("unsupported") != std::string::npos
        || normalized.find("unknown product") != std::string::npos
        || normalized.find("invalid product") != std::string::npos
        || normalized.find("product not found") != std::string::npos;
}

std::optional<std::string> error_detail(const json::object& message)
{
    for (const char* field : {"message", "error", "reason"})
    {
        const auto* value = message.if_contains(field);

        if (value && value->is_string())
        {
            return json_text(*value);
        }
    }

    return std::nullopt;
}

std::optional<Product> attributed_product(const json::object& message)
{
    if (const auto* value = message.if_contains("product_id"))
    {
        if (!value->is_string())
        {
            throw std::invalid_argument(
                "Malformed Coinbase error product_id"
            );
        }

        return Product::parse(json_text(*value));
    }

    if (const auto* values = message.if_contains("product_ids"))
    {
        if (!values->is_array() || values->as_array().size() != 1)
        {
            return std::nullopt;
        }

        return Product::parse(json_text(values->as_array().front()));
    }

    return std::nullopt;
}

std::optional<CoinbaseSubscriptionResult> subscription_result(
    std::string_view raw_message)
{
    json::object message;

    try
    {
        message = json::parse(raw_message).as_object();
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    bool is_error = false;

    for (const char* field : {"type", "channel"})
    {
        const auto* value = message.if_contains(field);
        is_error = is_error
            || (
                value
                && value->is_string()
                && value->as_string() == "error"
            );
    }

    if (!is_error)
    {
        return std::nullopt;
    }

    const std::string detail = error_detail(message).value_or(
        "Coinbase subscription rejected"
    );
    const std::optional<Product> product = attributed_product(message);

    if (product && permanent_product_rejection(detail))
    {
        return CoinbaseSubscriptionResult{
            CoinbaseSubscriptionDisposition::PermanentProductRejection,
            product,
            detail
        };
    }

    return CoinbaseSubscriptionResult{
        CoinbaseSubscriptionDisposition::SessionFailure,
        product,
        detail
    };
}

const MarketKey& event_market(const VenueSessionEvent& event)
{
    return std::visit(
        [](const auto& value) -> const MarketKey&
        {
            return value.market;
        },
        event
    );
}
}

CoinbaseUniverseSession::CoinbaseUniverseSession(
    MarketUniverse universe,
    std::unique_ptr<CoinbaseWire> wire)
    : universe_(std::move(universe)),
      wire_(std::move(wire))
{
    if (!wire_)
    {
        throw std::invalid_argument(
            "Coinbase universe session requires a wire"
        );
    }

    for (const Product& product : universe_.products())
    {
        books_.try_emplace(product);
        pending_.emplace_back(VenueMarketStatusEvent{
            {Venue::Coinbase, product},
            VenueMarketStatus::Connecting
        });
    }

    wire_->write(make_heartbeat_subscription());

    for (const Product& product : universe_.products())
    {
        wire_->write(make_level2_subscription(product.to_string()));
        pending_.emplace_back(VenueMarketStatusEvent{
            {Venue::Coinbase, product},
            VenueMarketStatus::WaitingForSnapshot
        });
    }
}

Venue CoinbaseUniverseSession::venue() const noexcept
{
    return Venue::Coinbase;
}

VenueSessionEvent CoinbaseUniverseSession::pop_pending()
{
    VenueSessionEvent event = std::move(pending_.front());
    pending_.pop_front();
    return event;
}

VenueSessionEvent CoinbaseUniverseSession::read_event()
{
    while (true)
    {
        if (!pending_.empty())
        {
            return pop_pending();
        }

        if (reconnect_required_)
        {
            throw std::runtime_error(
                "Coinbase universe session requires reconnect"
            );
        }

        process_frame(wire_->read());
    }
}

void CoinbaseUniverseSession::process_frame(std::string_view raw_message)
{
    if (const auto result = subscription_result(raw_message))
    {
        if (
            result->disposition
            == CoinbaseSubscriptionDisposition::PermanentProductRejection
        )
        {
            if (!result->product || !universe_.contains(*result->product))
            {
                throw std::runtime_error(
                    "Coinbase rejection names an unconfigured product"
                );
            }

            unsupported_.insert(*result->product);
            books_.at(*result->product).mark_desynchronized();
            pending_.emplace_back(VenueMarketStatusEvent{
                {Venue::Coinbase, *result->product},
                VenueMarketStatus::Unsupported
            });
            return;
        }

        throw std::runtime_error(
            "Coinbase universe subscription failed: " + result->detail
        );
    }

    const ParsedCoinbaseFrame frame = CoinbaseParser::parse_frame(
        std::string(raw_message)
    );
    const SequenceResult sequence = sequence_.observe(frame.sequence_num);

    if (sequence.status == SequenceStatus::Gap)
    {
        invalidate_all();
        reconnect_required_ = true;
        return;
    }

    for (const ParsedBookMessage& message : frame.book_messages)
    {
        const Product product = Product::parse(message.product_id);

        if (!universe_.contains(product) || unsupported_.contains(product))
        {
            throw std::runtime_error(
                "Coinbase frame contains an unexpected product: "
                + message.product_id
            );
        }

        BookReconstructor& reconstructor = books_.at(product);
        const ReconstructionResult result = reconstructor.process(
            message,
            sequence
        );

        if (!result.applied)
        {
            continue;
        }

        pending_.emplace_back(TrustedBookEvent{
            message.type == BookEventType::Snapshot
                ? TrustedBookEventType::Snapshot
                : TrustedBookEventType::Update,
            {Venue::Coinbase, product},
            message.timestamp,
            reconstructor.book()
        });
    }
}

void CoinbaseUniverseSession::invalidate_all()
{
    for (const Product& product : universe_.products())
    {
        if (unsupported_.contains(product))
        {
            continue;
        }

        books_.at(product).mark_desynchronized();
        pending_.emplace_back(TrustedBookEvent{
            TrustedBookEventType::Invalidated,
            {Venue::Coinbase, product},
            {},
            std::nullopt
        });
    }
}

CoinbaseUniverseRunner::CoinbaseUniverseRunner(
    MarketUniverse universe,
    VenueSessionEventSink sink,
    VenueUniverseSessionFactory factory,
    ReconnectBackoffConfig backoff,
    LiveSourceSleeper sleeper)
    : universe_(std::move(universe)),
      sink_(std::move(sink)),
      factory_(std::move(factory)),
      sleeper_(std::move(sleeper)),
      backoff_(backoff)
{
    if (!sink_ || !factory_)
    {
        throw std::invalid_argument(
            "Coinbase universe runner requires a sink and session factory"
        );
    }

    if (!sleeper_)
    {
        sleeper_ = [](std::chrono::milliseconds delay)
        {
            std::this_thread::sleep_for(delay);
        };
    }
}

CoinbaseUniverseRunner::CoinbaseUniverseRunner(
    MarketUniverse universe,
    VenueSessionEventSink sink,
    CoinbaseWireFactory factory,
    ReconnectBackoffConfig backoff,
    LiveSourceSleeper sleeper)
    : universe_(std::move(universe)),
      sink_(std::move(sink)),
      wire_factory_(std::move(factory)),
      sleeper_(std::move(sleeper)),
      backoff_(backoff)
{
    if (!sink_ || !wire_factory_)
    {
        throw std::invalid_argument(
            "Coinbase universe runner requires a sink and wire factory"
        );
    }

    if (!sleeper_)
    {
        sleeper_ = [](std::chrono::milliseconds delay)
        {
            std::this_thread::sleep_for(delay);
        };
    }
}

bool CoinbaseUniverseRunner::valid_market(
    const MarketKey& market) const noexcept
{
    return market.venue == Venue::Coinbase
        && universe_.contains(market.product);
}

void CoinbaseUniverseRunner::publish_recovery_status(
    VenueMarketStatus status)
{
    for (const Product& product : universe_.products())
    {
        if (!unsupported_.contains(product))
        {
            sink_(VenueMarketStatusEvent{
                {Venue::Coinbase, product},
                status
            });
        }
    }
}

std::unique_ptr<VenueUniverseSession>
CoinbaseUniverseRunner::make_session() const
{
    if (factory_)
    {
        return factory_();
    }

    std::vector<Product> active_products;
    active_products.reserve(universe_.size() - unsupported_.size());

    for (const Product& product : universe_.products())
    {
        if (!unsupported_.contains(product))
        {
            active_products.push_back(product);
        }
    }

    if (active_products.empty())
    {
        throw std::runtime_error(
            "Coinbase universe has no supported products"
        );
    }

    return std::make_unique<CoinbaseUniverseSession>(
        MarketUniverse(std::move(active_products)),
        wire_factory_()
    );
}

void CoinbaseUniverseRunner::run(LiveSourceStopCheck should_stop)
{
    if (!should_stop)
    {
        should_stop = []
        {
            return false;
        };
    }

    while (!should_stop())
    {
        try
        {
            std::unique_ptr<VenueUniverseSession> session = make_session();

            if (!session || session->venue() != Venue::Coinbase)
            {
                throw std::runtime_error(
                    "Coinbase session factory returned an invalid session"
                );
            }

            while (!should_stop())
            {
                VenueSessionEvent event = session->read_event();
                const MarketKey& market = event_market(event);

                if (!valid_market(market))
                {
                    throw std::runtime_error(
                        "Coinbase session emitted an unexpected market"
                    );
                }

                if (const auto* status =
                        std::get_if<VenueMarketStatusEvent>(&event))
                {
                    if (status->status == VenueMarketStatus::Unsupported)
                    {
                        unsupported_.insert(status->market.product);
                    }
                    else if (unsupported_.contains(status->market.product))
                    {
                        continue;
                    }
                }
                else if (unsupported_.contains(market.product))
                {
                    throw std::runtime_error(
                        "Coinbase session published a rejected market"
                    );
                }

                sink_(event);

                if (should_stop())
                {
                    return;
                }

                if (const auto* trusted = std::get_if<TrustedBookEvent>(&event))
                {
                    if (trusted->type == TrustedBookEventType::Snapshot)
                    {
                        backoff_.reset();
                    }
                }
            }

            return;
        }
        catch (const std::exception&)
        {
            if (should_stop())
            {
                return;
            }

            publish_recovery_status(VenueMarketStatus::Disconnected);

            if (unsupported_.size() == universe_.size())
            {
                return;
            }

            const std::chrono::milliseconds delay = backoff_.next_delay();
            publish_recovery_status(VenueMarketStatus::Reconnecting);
            sleeper_(delay);
        }
    }
}
