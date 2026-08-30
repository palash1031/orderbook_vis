#pragma once

#include "live_heatmap_engine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class LiveStreamSubscriber
{
public:
    std::optional<std::string> wait_for_message(
        std::chrono::milliseconds timeout
    );

private:
    friend class LiveStreamHub;

    void enqueue(std::string message);
    void replace_queue(std::vector<std::string> messages);

    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<std::string> messages_;
};

class LiveStreamHub
{
public:
    std::shared_ptr<LiveStreamSubscriber> subscribe();

    void publish(
        const LiveHeatmapEngine& engine,
        const LiveHeatmapResult& result
    );
    void publish_error(std::string_view message);

private:
    struct StoredColumn
    {
        std::size_t index;
        std::string column_json;
    };

    std::string hello_message(const LiveHeatmapEngine& engine) const;
    std::string state_message(
        LiveHeatmapStatus status,
        std::size_t position
    ) const;
    std::vector<std::string> snapshot_messages() const;
    void remove_expired_subscribers();
    void enqueue_to_all(const std::string& message);

    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<LiveStreamSubscriber>> subscribers_;
    std::deque<StoredColumn> columns_;
    std::string hello_message_;
    std::string state_message_;
    std::string error_message_;
    std::size_t max_columns_ = 0;
    bool ready_ = false;
};
