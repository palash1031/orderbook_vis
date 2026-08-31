#pragma once

#include "live_heatmap_engine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class LiveSourceStatus
{
    Connecting,
    Connected,
    Disconnected,
    Reconnecting
};

using LiveStreamSessionId = std::uint64_t;

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
    LiveStreamSessionId begin_product_session(std::string_view product_id);
    std::shared_ptr<LiveStreamSubscriber> subscribe();

    void publish(
        const LiveHeatmapEngine& engine,
        const LiveHeatmapResult& result
    );
    void publish(
        LiveStreamSessionId session_id,
        const LiveHeatmapEngine& engine,
        const LiveHeatmapResult& result
    );
    void publish_source_status(
        LiveSourceStatus status,
        std::string_view detail = {},
        std::chrono::milliseconds retry_delay = {}
    );
    void publish_source_status(
        LiveStreamSessionId session_id,
        LiveSourceStatus status,
        std::string_view detail = {},
        std::chrono::milliseconds retry_delay = {}
    );
    void publish_error(std::string_view message);
    void publish_error(
        LiveStreamSessionId session_id,
        std::string_view message
    );

private:
    struct StoredColumn
    {
        std::size_t index;
        std::string column_json;
    };

    std::string hello_message(const LiveHeatmapEngine& engine) const;
    std::string state_message() const;
    std::string product_reset_message() const;
    std::vector<std::string> snapshot_messages() const;
    std::vector<std::string> pending_messages() const;
    bool accepts_session(LiveStreamSessionId session_id) const noexcept;
    void publish_locked(
        const LiveHeatmapEngine& engine,
        const LiveHeatmapResult& result
    );
    void publish_source_status_locked(
        LiveSourceStatus status,
        std::string_view detail,
        std::chrono::milliseconds retry_delay
    );
    void publish_error_locked(std::string_view message);
    void refresh_state_message();
    void remove_expired_subscribers();
    void enqueue_to_all(const std::string& message);

    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<LiveStreamSubscriber>> subscribers_;
    std::deque<StoredColumn> columns_;
    std::string hello_message_;
    std::string state_message_;
    std::string error_message_;
    std::string source_detail_;
    std::string product_id_;
    std::size_t max_columns_ = 0;
    std::size_t position_ = 0;
    LiveStreamSessionId session_id_ = 0;
    std::chrono::milliseconds retry_delay_{};
    LiveSourceStatus source_status_ = LiveSourceStatus::Connecting;
    LiveHeatmapStatus book_status_ = LiveHeatmapStatus::WaitingForSnapshot;
    bool ready_ = false;
};
