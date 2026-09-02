#include "live_heatmap_engine.hpp"

#include "book_reconstructor.hpp"
#include "coinbase_parser.hpp"
#include "heatmap_json.hpp"
#include "recorder_config.hpp"
#include "sequence_tracker.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

LiveHeatmapEngine::LiveHeatmapEngine(
    std::string_view product_id,
    HeatmapConfig heatmap_config)
    : product_id_(normalize_product_id(product_id)),
      heatmap_(std::move(heatmap_config))
{
}

LiveHeatmapResult LiveHeatmapEngine::process(std::string_view raw_message)
{
    LiveHeatmapResult live_result;
    const ParsedCoinbaseMessage parsed = CoinbaseParser::parse_message(
        std::string(raw_message)
    );
    const SequenceResult sequence = sequence_tracker_.observe(
        parsed.sequence_num
    );

    if (!parsed.book_message)
    {
        return live_result;
    }

    const ParsedBookMessage& book_message = *parsed.book_message;

    if (book_message.product_id != product_id_)
    {
        throw std::invalid_argument(
            "Live feed product does not match configured product"
        );
    }

    const bool is_snapshot = book_message.type == BookEventType::Snapshot;

    if (sequence.status == SequenceStatus::Gap)
    {
        reconstructor_.mark_desynchronized();
        heatmap_.mark_discontinuity();

        if (!is_snapshot)
        {
            set_status(LiveHeatmapStatus::Gap, live_result);
        }
    }

    const ReconstructionResult reconstruction = reconstructor_.process(
        book_message,
        sequence
    );

    if (
        reconstruction.applied
        && is_snapshot
        && sequence.status == SequenceStatus::Gap
    )
    {
        sequence_tracker_.reset();
        sequence_tracker_.observe(parsed.sequence_num);
    }

    if (!reconstruction.applied)
    {
        return live_result;
    }

    if (!heatmap_.sample(book_message.timestamp, reconstructor_.book()))
    {
        return live_result;
    }

    set_status(LiveHeatmapStatus::Live, live_result);
    collect_columns(live_result);
    return live_result;
}

LiveHeatmapResult LiveHeatmapEngine::process(const TrustedBookEvent& event)
{
    if (event.market.product.to_string() != product_id_)
    {
        throw std::invalid_argument(
            "Trusted book product does not match configured product"
        );
    }

    if (event.type == TrustedBookEventType::Invalidated)
    {
        return begin_recovery();
    }

    if (
        event.type == TrustedBookEventType::Update
        && status_ == LiveHeatmapStatus::WaitingForSnapshot
    )
    {
        return {};
    }

    if (!event.book)
    {
        throw std::invalid_argument("Trusted book event has no book state");
    }

    LiveHeatmapResult result;

    if (!heatmap_.sample(event.timestamp, *event.book))
    {
        return result;
    }

    set_status(LiveHeatmapStatus::Live, result);
    collect_columns(result);
    return result;
}

LiveHeatmapResult LiveHeatmapEngine::begin_recovery()
{
    LiveHeatmapResult result;
    sequence_tracker_.reset();
    reconstructor_.mark_desynchronized();
    heatmap_.mark_discontinuity();
    set_status(LiveHeatmapStatus::WaitingForSnapshot, result);
    return result;
}

const std::string& LiveHeatmapEngine::product_id() const noexcept
{
    return product_id_;
}

LiveHeatmapStatus LiveHeatmapEngine::status() const noexcept
{
    return status_;
}

const HeatmapHistory& LiveHeatmapEngine::heatmap() const noexcept
{
    return heatmap_;
}

double LiveHeatmapEngine::peak_depth() const noexcept
{
    return peak_depth_;
}

void LiveHeatmapEngine::set_status(
    LiveHeatmapStatus status,
    LiveHeatmapResult& result)
{
    if (status_ == status)
    {
        return;
    }

    status_ = status;
    result.status_change = status;
}

void LiveHeatmapEngine::collect_columns(LiveHeatmapResult& result)
{
    const auto& columns = heatmap_.columns();

    if (columns.empty())
    {
        return;
    }

    const auto add_column = [this, &result](
        const HeatmapColumn& column,
        bool replaces_existing)
    {
        const std::size_t index = replaces_existing
            ? next_column_index_ - 1
            : next_column_index_++;

        for (const double quantity : column.bid_quantities)
        {
            peak_depth_ = std::max(peak_depth_, quantity);
        }

        for (const double quantity : column.ask_quantities)
        {
            peak_depth_ = std::max(peak_depth_, quantity);
        }

        result.columns.push_back({
            index,
            replaces_existing,
            serialize_heatmap_column(column)
        });
        last_emitted_timestamp_ = column.timestamp;
    };

    if (!last_emitted_timestamp_)
    {
        for (const HeatmapColumn& column : columns)
        {
            add_column(column, false);
        }

        return;
    }

    if (columns.back().timestamp == *last_emitted_timestamp_)
    {
        add_column(columns.back(), true);
        return;
    }

    for (const HeatmapColumn& column : columns)
    {
        if (column.timestamp > *last_emitted_timestamp_)
        {
            add_column(column, false);
        }
    }
}
