#pragma once

#include "book_reconstructor.hpp"
#include "heatmap_history.hpp"
#include "sequence_tracker.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class LiveHeatmapStatus
{
    WaitingForSnapshot,
    Live,
    Gap
};

struct LiveHeatmapColumnUpdate
{
    std::size_t index;
    bool replaces_existing;
    std::string column_json;
};

struct LiveHeatmapResult
{
    std::optional<LiveHeatmapStatus> status_change;
    std::vector<LiveHeatmapColumnUpdate> columns;
};

class LiveHeatmapEngine
{
public:
    LiveHeatmapEngine(
        std::string_view product_id,
        HeatmapConfig heatmap_config = {}
    );

    LiveHeatmapResult process(std::string_view raw_message);

    const std::string& product_id() const noexcept;
    LiveHeatmapStatus status() const noexcept;
    const HeatmapHistory& heatmap() const noexcept;
    double peak_depth() const noexcept;

private:
    void set_status(
        LiveHeatmapStatus status,
        LiveHeatmapResult& result
    );
    void collect_columns(LiveHeatmapResult& result);

    std::string product_id_;
    SequenceTracker sequence_tracker_;
    BookReconstructor reconstructor_;
    HeatmapHistory heatmap_;
    LiveHeatmapStatus status_ = LiveHeatmapStatus::WaitingForSnapshot;
    std::optional<MarketTimestamp> last_emitted_timestamp_;
    std::size_t next_column_index_ = 0;
    double peak_depth_ = 0.0;
};
