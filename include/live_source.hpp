#pragma once

#include "heatmap_history.hpp"
#include "live_message_source.hpp"
#include "live_stream.hpp"
#include "reconnect_backoff.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

using LiveMessageSourceFactory =
    std::function<std::unique_ptr<LiveMessageSource>()>;
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
        LiveSourceSleeper sleeper = {}
    );

    void run(LiveSourceStopCheck should_stop = {});

private:
    std::string product_id_;
    LiveHeatmapEngine engine_;
    std::shared_ptr<LiveStreamHub> hub_;
    LiveMessageSourceFactory source_factory_;
    LiveSourceSleeper sleeper_;
    ReconnectBackoff backoff_;
};
