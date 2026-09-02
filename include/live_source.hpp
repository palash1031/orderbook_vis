#pragma once

#include "heatmap_history.hpp"
#include "live_message_source.hpp"
#include "live_stream.hpp"
#include "reconnect_backoff.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

using LiveMessageSourceFactory =
    std::function<std::unique_ptr<LiveMessageSource>()>;
using LiveProductSourceFactory =
    std::function<std::unique_ptr<LiveMessageSource>(std::string_view)>;
using LiveSourceSleeper =
    std::function<void(std::chrono::milliseconds)>;
using LiveSourceStopCheck = std::function<bool()>;

class LiveSourceRunner
{
public:
    LiveSourceRunner(
        std::string_view product_id,
        HeatmapConfig heatmap_config,
        std::shared_ptr<LiveStreamHub> hub,
        LiveMessageSourceFactory source_factory,
        ReconnectBackoffConfig backoff_config = {},
        LiveSourceSleeper sleeper = {},
        std::string_view source_name = "Coinbase"
    );
    LiveSourceRunner(
        LiveStreamSessionId session_id,
        std::string_view product_id,
        HeatmapConfig heatmap_config,
        std::shared_ptr<LiveStreamHub> hub,
        LiveMessageSourceFactory source_factory,
        ReconnectBackoffConfig backoff_config = {},
        LiveSourceSleeper sleeper = {},
        std::string_view source_name = "Coinbase"
    );

    void run(LiveSourceStopCheck should_stop = {});

private:
    LiveStreamSessionId session_id_ = 0;
    std::string product_id_;
    LiveHeatmapEngine engine_;
    std::shared_ptr<LiveStreamHub> hub_;
    LiveMessageSourceFactory source_factory_;
    LiveSourceSleeper sleeper_;
    ReconnectBackoff backoff_;
    std::string source_name_;
};

class LiveMarketService
{
public:
    LiveMarketService(
        std::string_view initial_product_id,
        HeatmapConfig heatmap_config,
        std::shared_ptr<LiveStreamHub> hub,
        LiveProductSourceFactory source_factory,
        ReconnectBackoffConfig backoff_config = {},
        LiveSourceSleeper sleeper = {},
        std::string_view source_name = "Coinbase"
    );

    void run(LiveSourceStopCheck should_stop = {});
    bool switch_product(std::string_view product_id);
    void apply_control(std::string_view command_json);
    std::string product_id() const;

private:
    struct Selection
    {
        std::string product_id;
        LiveStreamSessionId session_id = 0;
    };

    bool is_current_session(LiveStreamSessionId session_id) const;

    HeatmapConfig heatmap_config_;
    std::shared_ptr<LiveStreamHub> hub_;
    LiveProductSourceFactory source_factory_;
    ReconnectBackoffConfig backoff_config_;
    LiveSourceSleeper sleeper_;
    std::string source_name_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Selection selection_;
};
