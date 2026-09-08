#include "kraken_universe_session.hpp"

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
std::string json_text(const json::value& value)
{
    const auto& text = value.as_string();
    return std::string(text.c_str(), text.size());
}

json::object parse_object(std::string_view raw_message)
{
    try
    {
        return json::parse(raw_message).as_object();
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            std::string("Malformed Kraken universe message: ")
            + error.what()
        );
    }
}

std::string channel(const json::object& message)
{
    const auto* value = message.if_contains("channel");

    if (!value)
    {
        return {};
    }

    if (!value->is_string())
    {
        throw std::runtime_error("Malformed Kraken universe channel");
    }

    return json_text(*value);
}

bool is_subscription_acknowledgement(const json::object& message)
{
    const auto* method = message.if_contains("method");
    return method
        && method->is_string()
        && method->as_string() == "subscribe";
}

const json::object& acknowledgement_result(const json::object& message)
{
    const auto* result = message.if_contains("result");

    if (!result || !result->is_object())
    {
        throw std::runtime_error(
            "Malformed Kraken subscription acknowledgement"
        );
    }

    return result->as_object();
}

std::string acknowledgement_channel(const json::object& message)
{
    const json::object& result = acknowledgement_result(message);
    const auto* value = result.if_contains("channel");

    if (!value || !value->is_string())
    {
        throw std::runtime_error(
            "Malformed Kraken subscription acknowledgement channel"
        );
    }

    return json_text(*value);
}

bool acknowledgement_success(const json::object& message)
{
    const auto* success = message.if_contains("success");

    if (!success || !success->is_bool())
    {
        throw std::runtime_error(
            "Malformed Kraken subscription acknowledgement result"
        );
    }

    return success->as_bool();
}

std::string acknowledgement_error(const json::object& message)
{
    const auto* error = message.if_contains("error");

    if (!error)
    {
        return "Kraken subscription rejected";
    }

    if (!error->is_string())
    {
        throw std::runtime_error(
            "Malformed Kraken subscription acknowledgement error"
        );
    }

    return json_text(*error);
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

bool permanent_market_rejection(std::string_view detail)
{
    const std::string normalized = lowercase(detail);
    return normalized.find("pair not supported") != std::string::npos
        || normalized.find("symbol not supported") != std::string::npos
        || normalized.find("unknown symbol") != std::string::npos;
}

std::string book_symbol(const json::object& message)
{
    const auto* data = message.if_contains("data");

    if (!data || !data->is_array() || data->as_array().size() != 1)
    {
        throw std::runtime_error(
            "Kraken book message must contain exactly one product"
        );
    }

    const json::object& payload = data->as_array().front().as_object();
    const auto* symbol = payload.if_contains("symbol");

    if (!symbol || !symbol->is_string())
    {
        throw std::runtime_error("Malformed Kraken book symbol");
    }

    return json_text(*symbol);
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

KrakenUniverseSession::KrakenUniverseSession(
    MarketUniverse universe,
    std::unique_ptr<KrakenWire> wire,
    std::size_t depth)
    : universe_(std::move(universe)),
      wire_(std::move(wire)),
      depth_(depth)
{
    if (!wire_)
    {
        throw std::invalid_argument(
            "Kraken universe session requires a wire"
        );
    }

    if (!is_supported_kraken_book_depth(depth_))
    {
        throw std::invalid_argument("Unsupported Kraken book depth");
    }

    for (const Product& product : universe_.products())
    {
        pending_.emplace_back(VenueMarketStatusEvent{
            {Venue::Kraken, product},
            VenueMarketStatus::Connecting
        });
    }

    initialize();
}

Venue KrakenUniverseSession::venue() const noexcept
{
    return Venue::Kraken;
}

void KrakenUniverseSession::initialize()
{
    wire_->write(make_kraken_instrument_subscription());

    while (true)
    {
        const std::string raw_message = wire_->read();
        const json::object message = parse_object(raw_message);

        if (is_subscription_acknowledgement(message))
        {
            if (acknowledgement_channel(message) != "instrument")
            {
                throw std::runtime_error(
                    "Unexpected Kraken acknowledgement during discovery"
                );
            }

            if (!acknowledgement_success(message))
            {
                throw std::runtime_error(acknowledgement_error(message));
            }

            continue;
        }

        const std::string message_channel = channel(message);

        if (message_channel == "status" || message_channel == "heartbeat")
        {
            continue;
        }

        if (message_channel != "instrument")
        {
            throw std::runtime_error(
                "Expected Kraken instrument discovery metadata"
            );
        }

        configure(KrakenInstrumentCatalog::parse(raw_message));
        return;
    }
}

void KrakenUniverseSession::configure(
    const KrakenInstrumentCatalog& catalog)
{
    std::vector<std::string> native_symbols;

    for (const Product& product : universe_.products())
    {
        const std::optional<KrakenInstrument> instrument =
            catalog.instrument(product);

        if (!instrument || instrument->status != "online")
        {
            unsupported_.insert(product);
            pending_.emplace_back(VenueMarketStatusEvent{
                {Venue::Kraken, product},
                VenueMarketStatus::Unsupported
            });
            continue;
        }

        if (!product_by_native_.emplace(
                instrument->native_symbol,
                product
            ).second)
        {
            throw std::runtime_error(
                "Ambiguous Kraken native symbol dispatch"
            );
        }

        supported_.insert(product);
        adapters_.try_emplace(
            product,
            product,
            instrument->native_symbol,
            depth_
        );
        native_symbols.push_back(instrument->native_symbol);
    }

    if (native_symbols.empty())
    {
        reconnect_required_ = true;
        return;
    }

    wire_->write(make_kraken_book_subscription(native_symbols, depth_));
}

VenueSessionEvent KrakenUniverseSession::pop_pending()
{
    VenueSessionEvent event = std::move(pending_.front());
    pending_.pop_front();
    return event;
}

void KrakenUniverseSession::process_acknowledgement(
    const std::string& raw_message)
{
    const json::object message = parse_object(raw_message);

    if (acknowledgement_channel(message) != "book")
    {
        return;
    }

    const json::object& result = acknowledgement_result(message);
    const auto* symbol_value = result.if_contains("symbol");

    if (!symbol_value || !symbol_value->is_string())
    {
        throw std::runtime_error(
            "Kraken book acknowledgement requires a symbol"
        );
    }

    const std::string symbol = json_text(*symbol_value);
    const auto product = product_by_native_.find(symbol);

    if (product == product_by_native_.end())
    {
        throw std::runtime_error(
            "Kraken acknowledgement names an unknown symbol"
        );
    }

    if (acknowledgement_success(message))
    {
        if (
            acknowledged_.insert(product->second).second
            && adapters_.at(product->second).status()
                != VenueMarketStatus::Live
        )
        {
            pending_.emplace_back(VenueMarketStatusEvent{
                {Venue::Kraken, product->second},
                VenueMarketStatus::WaitingForSnapshot
            });
        }

        return;
    }

    const std::string detail = acknowledgement_error(message);

    if (!permanent_market_rejection(detail))
    {
        throw std::runtime_error(detail);
    }

    supported_.erase(product->second);
    unsupported_.insert(product->second);
    adapters_.at(product->second).reset();
    pending_.emplace_back(VenueMarketStatusEvent{
        {Venue::Kraken, product->second},
        VenueMarketStatus::Unsupported
    });
}

VenueSessionEvent KrakenUniverseSession::read_event()
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
                "Kraken universe session requires reconnect"
            );
        }

        const std::string raw_message = wire_->read();
        const json::object message = parse_object(raw_message);

        if (is_subscription_acknowledgement(message))
        {
            process_acknowledgement(raw_message);
            continue;
        }

        const std::string message_channel = channel(message);

        if (message_channel != "book")
        {
            continue;
        }

        const std::string symbol = book_symbol(message);
        const auto product = product_by_native_.find(symbol);

        if (product == product_by_native_.end())
        {
            throw std::runtime_error(
                "Kraken book message names an unknown symbol"
            );
        }

        if (unsupported_.contains(product->second))
        {
            throw std::runtime_error(
                "Kraken published a rejected market"
            );
        }

        const std::optional<TrustedBookEvent> event =
            adapters_.at(product->second).process(raw_message);

        if (!event)
        {
            continue;
        }

        if (event->type == TrustedBookEventType::Invalidated)
        {
            reconnect_required_ = true;
        }

        return *event;
    }
}

KrakenUniverseRunner::KrakenUniverseRunner(
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
            "Kraken universe runner requires a sink and session factory"
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

KrakenUniverseRunner::KrakenUniverseRunner(
    MarketUniverse universe,
    VenueSessionEventSink sink,
    KrakenWireFactory factory,
    std::size_t depth,
    ReconnectBackoffConfig backoff,
    LiveSourceSleeper sleeper)
    : universe_(std::move(universe)),
      sink_(std::move(sink)),
      wire_factory_(std::move(factory)),
      depth_(depth),
      sleeper_(std::move(sleeper)),
      backoff_(backoff)
{
    if (!sink_ || !wire_factory_)
    {
        throw std::invalid_argument(
            "Kraken universe runner requires a sink and wire factory"
        );
    }

    if (!is_supported_kraken_book_depth(depth_))
    {
        throw std::invalid_argument("Unsupported Kraken book depth");
    }

    if (!sleeper_)
    {
        sleeper_ = [](std::chrono::milliseconds delay)
        {
            std::this_thread::sleep_for(delay);
        };
    }
}

bool KrakenUniverseRunner::valid_market(
    const MarketKey& market) const noexcept
{
    return market.venue == Venue::Kraken
        && universe_.contains(market.product);
}

void KrakenUniverseRunner::publish_recovery_status(
    VenueMarketStatus status)
{
    for (const Product& product : universe_.products())
    {
        if (!unsupported_.contains(product))
        {
            sink_(VenueMarketStatusEvent{
                {Venue::Kraken, product},
                status
            });
        }
    }
}

std::unique_ptr<VenueUniverseSession>
KrakenUniverseRunner::make_session() const
{
    if (factory_)
    {
        return factory_();
    }

    return std::make_unique<KrakenUniverseSession>(
        universe_,
        wire_factory_(),
        depth_
    );
}

void KrakenUniverseRunner::run(LiveSourceStopCheck should_stop)
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

            if (!session || session->venue() != Venue::Kraken)
            {
                throw std::runtime_error(
                    "Kraken session factory returned an invalid session"
                );
            }

            while (!should_stop())
            {
                VenueSessionEvent event = session->read_event();
                const MarketKey& market = event_market(event);

                if (!valid_market(market))
                {
                    throw std::runtime_error(
                        "Kraken session emitted an unexpected market"
                    );
                }

                if (const auto* status =
                        std::get_if<VenueMarketStatusEvent>(&event))
                {
                    if (status->status == VenueMarketStatus::Unsupported)
                    {
                        unsupported_.insert(status->market.product);
                    }
                    else if (status->status == VenueMarketStatus::Connecting)
                    {
                        unsupported_.erase(status->market.product);
                    }
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
