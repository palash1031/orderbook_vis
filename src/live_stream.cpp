#include "live_stream.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace json = boost::json;

namespace
{
const char* source_status_name(LiveSourceStatus status)
{
    switch (status)
    {
        case LiveSourceStatus::Connecting:
            return "connecting";
        case LiveSourceStatus::Connected:
            return "connected";
        case LiveSourceStatus::Disconnected:
            return "disconnected";
        case LiveSourceStatus::Reconnecting:
            return "reconnecting";
    }

    return "connecting";
}

const char* book_status_name(LiveHeatmapStatus status)
{
    switch (status)
    {
        case LiveHeatmapStatus::WaitingForSnapshot:
            return "waiting_for_snapshot";
        case LiveHeatmapStatus::Live:
            return "live";
        case LiveHeatmapStatus::Gap:
            return "gap";
    }

    return "waiting_for_snapshot";
}

const char* effective_status_name(
    LiveSourceStatus source_status,
    LiveHeatmapStatus book_status)
{
    switch (source_status)
    {
        case LiveSourceStatus::Connecting:
            return "connecting";
        case LiveSourceStatus::Disconnected:
            return "disconnected";
        case LiveSourceStatus::Reconnecting:
            return "reconnecting";
        case LiveSourceStatus::Connected:
            return book_status_name(book_status);
    }

    return "connecting";
}

std::string column_message(
    std::size_t index,
    bool replaces_existing,
    std::string_view column_json)
{
    return R"({"type":"column","index":)"
        + std::to_string(index)
        + R"(,"replace":)"
        + (replaces_existing ? "true" : "false")
        + R"(,"column":)"
        + std::string(column_json)
        + '}';
}

std::string protocol_error(std::string_view message)
{
    json::object payload;
    payload["type"] = "error";
    payload["message"] = message;
    return json::serialize(payload);
}
}

std::optional<std::string> LiveStreamSubscriber::wait_for_message(
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

void LiveStreamSubscriber::enqueue(std::string message)
{
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(message));
    }

    available_.notify_one();
}

void LiveStreamSubscriber::replace_queue(std::vector<std::string> messages)
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

std::shared_ptr<LiveStreamSubscriber> LiveStreamHub::subscribe()
{
    auto subscriber = std::make_shared<LiveStreamSubscriber>();
    std::lock_guard lock(mutex_);
    remove_expired_subscribers();
    subscribers_.push_back(subscriber);

    subscriber->replace_queue(
        ready_ ? snapshot_messages() : pending_messages()
    );

    return subscriber;
}

void LiveStreamHub::publish(
    const LiveHeatmapEngine& engine,
    const LiveHeatmapResult& result)
{
    if (!result.status_change && result.columns.empty())
    {
        return;
    }

    std::lock_guard lock(mutex_);
    remove_expired_subscribers();

    for (const LiveHeatmapColumnUpdate& update : result.columns)
    {
        if (update.replaces_existing)
        {
            if (columns_.empty() || columns_.back().index != update.index)
            {
                throw std::logic_error(
                    "Live stream replacement does not match current column"
                );
            }

            columns_.back().column_json = update.column_json;
        }
        else
        {
            columns_.push_back({update.index, update.column_json});
        }
    }

    const HeatmapConfig& config = engine.heatmap().config();
    max_columns_ = config.max_columns;

    while (columns_.size() > max_columns_)
    {
        columns_.pop_front();
    }

    const bool became_ready = !ready_
        && !columns_.empty()
        && engine.heatmap().resolved_price_bin_size().has_value();

    if (became_ready)
    {
        ready_ = true;
    }

    if (ready_)
    {
        hello_message_ = hello_message(engine);
        position_ = columns_.empty()
            ? 0
            : columns_.back().index + 1;
    }

    book_status_ = engine.status();
    refresh_state_message();

    if (became_ready)
    {
        const std::vector<std::string> messages = snapshot_messages();

        for (const auto& weak_subscriber : subscribers_)
        {
            if (const auto subscriber = weak_subscriber.lock())
            {
                subscriber->replace_queue(messages);
            }
        }

        return;
    }

    if (result.status_change && ready_)
    {
        enqueue_to_all(state_message_);
    }

    if (!ready_)
    {
        return;
    }

    for (const LiveHeatmapColumnUpdate& update : result.columns)
    {
        enqueue_to_all(column_message(
            update.index,
            update.replaces_existing,
            update.column_json
        ));
    }
}

void LiveStreamHub::publish_source_status(
    LiveSourceStatus status,
    std::string_view detail,
    std::chrono::milliseconds retry_delay)
{
    std::lock_guard lock(mutex_);
    remove_expired_subscribers();
    source_status_ = status;
    source_detail_ = detail;
    retry_delay_ = retry_delay;

    if (
        status == LiveSourceStatus::Connecting
        || status == LiveSourceStatus::Connected
    )
    {
        error_message_.clear();
    }

    refresh_state_message();
    enqueue_to_all(state_message_);
}

void LiveStreamHub::publish_error(std::string_view message)
{
    std::lock_guard lock(mutex_);
    remove_expired_subscribers();
    error_message_ = protocol_error(message);
    enqueue_to_all(error_message_);
}

std::string LiveStreamHub::hello_message(
    const LiveHeatmapEngine& engine) const
{
    const HeatmapHistory& heatmap = engine.heatmap();
    const HeatmapConfig& config = heatmap.config();
    const auto resolved_bin = heatmap.resolved_price_bin_size();

    if (!resolved_bin || heatmap.columns().empty())
    {
        throw std::logic_error("Live stream metadata is unresolved");
    }

    json::object serialized_config;
    serialized_config["time_bucket_ns"] = config.time_bucket.count();
    serialized_config["price_bin_size"] = *resolved_bin;
    serialized_config["price_bin_count"] = config.price_bin_count;
    serialized_config["max_columns"] = config.max_columns;

    const auto start_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            heatmap.columns().front().timestamp.time_since_epoch()
        ).count();

    json::object hello;
    hello["type"] = "hello";
    hello["schema_version"] = 1;
    hello["stream_mode"] = "live";
    hello["product_id"] = engine.product_id();
    hello["config"] = std::move(serialized_config);
    hello["first_column_index"] = columns_.front().index;
    hello["start_timestamp_ms"] = start_milliseconds;
    hello["peak_depth"] = engine.peak_depth();
    return json::serialize(hello);
}

std::string LiveStreamHub::state_message() const
{
    json::object state;
    state["type"] = "state";
    state["stream_mode"] = "live";
    state["status"] = effective_status_name(
        source_status_,
        book_status_
    );
    state["source_status"] = source_status_name(source_status_);
    state["book_status"] = book_status_name(book_status_);
    state["speed"] = 1;
    state["position"] = position_;

    if (!source_detail_.empty())
    {
        state["message"] = source_detail_;
    }

    if (retry_delay_.count() > 0)
    {
        state["retry_ms"] = retry_delay_.count();
    }

    return json::serialize(state);
}

std::vector<std::string> LiveStreamHub::snapshot_messages() const
{
    std::vector<std::string> messages;
    messages.reserve(columns_.size() + 3);
    messages.push_back(hello_message_);

    for (const StoredColumn& column : columns_)
    {
        messages.push_back(column_message(
            column.index,
            false,
            column.column_json
        ));
    }

    messages.push_back(state_message_);

    if (!error_message_.empty())
    {
        messages.push_back(error_message_);
    }

    return messages;
}

std::vector<std::string> LiveStreamHub::pending_messages() const
{
    std::vector<std::string> messages;
    messages.push_back(
        state_message_.empty() ? state_message() : state_message_
    );

    if (!error_message_.empty())
    {
        messages.push_back(error_message_);
    }

    return messages;
}

void LiveStreamHub::refresh_state_message()
{
    state_message_ = state_message();
}

void LiveStreamHub::remove_expired_subscribers()
{
    std::erase_if(subscribers_, [](const auto& subscriber)
    {
        return subscriber.expired();
    });
}

void LiveStreamHub::enqueue_to_all(const std::string& message)
{
    for (const auto& weak_subscriber : subscribers_)
    {
        if (const auto subscriber = weak_subscriber.lock())
        {
            subscriber->enqueue(message);
        }
    }
}
