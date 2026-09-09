#include "scanner_stream.hpp"

#include <boost/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace json = boost::json;

namespace
{
constexpr Venue scanner_venues[]{Venue::Coinbase, Venue::Kraken};

json::value nullable_number(const std::optional<VenueQuote>& quote)
{
    return quote
        ? json::value(quote->price)
        : json::value(nullptr);
}

json::value nullable_quantity(const std::optional<VenueQuote>& quote)
{
    return quote
        ? json::value(quote->quantity)
        : json::value(nullptr);
}

json::object venue_message(const VenueTopOfBook& top)
{
    json::object venue;
    venue["status"] = venue_market_status_name(top.status);
    venue["bid"] = nullable_number(top.bid);
    venue["bid_quantity"] = nullable_quantity(top.bid);
    venue["ask"] = nullable_number(top.ask);
    venue["ask_quantity"] = nullable_quantity(top.ask);
    return venue;
}

void set_consolidated_side(
    json::object& message,
    std::string_view price_key,
    std::string_view quantity_key,
    std::string_view venue_key,
    const std::optional<VenueQuote>& quote)
{
    if (!quote)
    {
        message[price_key] = nullptr;
        message[quantity_key] = nullptr;
        message[venue_key] = nullptr;
        return;
    }

    message[price_key] = quote->price;
    message[quantity_key] = quote->quantity;
    message[venue_key] = venue_name(quote->venue);
}

std::string scanner_update_message(const ConsolidatedQuote& quote)
{
    json::object venues;

    for (const Venue venue : scanner_venues)
    {
        const auto top = quote.venues.find(venue);

        if (top == quote.venues.end())
        {
            throw std::invalid_argument(
                "Scanner quote requires every configured venue"
            );
        }

        venues[venue_name(venue)] = venue_message(top->second);
    }

    if (quote.venues.size() != std::size(scanner_venues))
    {
        throw std::invalid_argument(
            "Scanner quote contains an unexpected venue"
        );
    }

    json::object consolidated;
    set_consolidated_side(
        consolidated,
        "best_bid",
        "best_bid_quantity",
        "best_bid_venue",
        quote.best_bid
    );
    set_consolidated_side(
        consolidated,
        "best_ask",
        "best_ask_quantity",
        "best_ask_venue",
        quote.best_ask
    );

    json::value fragmentation = nullptr;

    if (quote.fragmentation)
    {
        json::object metrics;
        metrics["bid_bps"] = quote.fragmentation->bid_difference_bps;
        metrics["ask_bps"] = quote.fragmentation->ask_difference_bps;
        metrics["max_bps"] = quote.fragmentation->max_difference_bps;
        fragmentation = std::move(metrics);
    }

    json::object update;
    update["type"] = "scanner_update";
    update["product_id"] = quote.product.to_string();
    update["venues"] = std::move(venues);
    update["consolidated"] = std::move(consolidated);
    update["fragmentation"] = std::move(fragmentation);
    return json::serialize(update);
}

std::string scanner_hello_message(const MarketUniverse& universe)
{
    json::array venues;

    for (const Venue venue : scanner_venues)
    {
        venues.emplace_back(venue_name(venue));
    }

    json::array products;

    for (const Product& product : universe.products())
    {
        products.emplace_back(product.to_string());
    }

    json::object hello;
    hello["type"] = "scanner_hello";
    hello["schema_version"] = 1;
    hello["venues"] = std::move(venues);
    hello["products"] = std::move(products);
    return json::serialize(hello);
}

ConsolidatedQuote initial_quote(const Product& product)
{
    std::map<Venue, VenueTopOfBook> venues;

    for (const Venue venue : scanner_venues)
    {
        venues.emplace(
            venue,
            VenueTopOfBook{
                VenueMarketStatus::Connecting,
                std::nullopt,
                std::nullopt
            }
        );
    }

    return {
        product,
        std::move(venues),
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
}
}

std::optional<std::string> ScannerStreamSubscriber::wait_for_message(
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    available_.wait_for(lock, timeout, [this]
    {
        return !messages_.empty();
    });

    if (messages_.empty())
    {
        return std::nullopt;
    }

    std::string message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

void ScannerStreamSubscriber::enqueue(std::string message)
{
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(message));
    }

    available_.notify_one();
}

void ScannerStreamSubscriber::replace_queue(
    std::vector<std::string> messages)
{
    {
        std::lock_guard lock(mutex_);
        messages_.clear();

        for (std::string& message : messages)
        {
            messages_.push_back(std::move(message));
        }
    }

    available_.notify_one();
}

ScannerStreamHub::ScannerStreamHub(MarketUniverse universe)
    : universe_(std::move(universe)),
      hello_message_(scanner_hello_message(universe_))
{
    for (const Product& product : universe_.products())
    {
        current_updates_.emplace(
            product,
            scanner_update_message(initial_quote(product))
        );
    }
}

std::shared_ptr<ScannerStreamSubscriber> ScannerStreamHub::subscribe()
{
    auto subscriber = std::make_shared<ScannerStreamSubscriber>();
    std::lock_guard lock(mutex_);
    remove_expired_subscribers();
    subscribers_.push_back(subscriber);
    subscriber->replace_queue(snapshot_messages());
    return subscriber;
}

void ScannerStreamHub::publish(const ConsolidatedQuote& quote)
{
    if (!universe_.contains(quote.product))
    {
        throw std::invalid_argument(
            "Scanner quote product is outside the configured universe"
        );
    }

    const std::string message = scanner_update_message(quote);
    std::lock_guard lock(mutex_);
    const auto current = current_updates_.find(quote.product);

    if (current != current_updates_.end() && current->second == message)
    {
        return;
    }

    current_updates_.insert_or_assign(quote.product, message);
    remove_expired_subscribers();
    enqueue_to_all(message);
}

std::vector<std::string> ScannerStreamHub::snapshot_messages() const
{
    std::vector<std::string> messages;
    messages.reserve(universe_.size() + 1);
    messages.push_back(hello_message_);

    for (const Product& product : universe_.products())
    {
        messages.push_back(current_updates_.at(product));
    }

    return messages;
}

void ScannerStreamHub::remove_expired_subscribers()
{
    std::erase_if(subscribers_, [](const auto& subscriber)
    {
        return subscriber.expired();
    });
}

void ScannerStreamHub::enqueue_to_all(const std::string& message)
{
    for (const auto& weak_subscriber : subscribers_)
    {
        if (const auto subscriber = weak_subscriber.lock())
        {
            subscriber->enqueue(message);
        }
    }
}
