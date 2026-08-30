#include "heatmap_history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
constexpr double automatic_price_span_fraction = 0.0025;

double stable_floor(double value)
{
    const double nearest_integer = std::round(value);
    const double tolerance =
        8.0
        * std::numeric_limits<double>::epsilon()
        * std::max(1.0, std::abs(value));

    if (std::abs(value - nearest_integer) <= tolerance)
    {
        return nearest_integer;
    }

    return std::floor(value);
}

void accumulate_levels(
    const OrderBook::Levels& levels,
    double first_bin,
    double price_bin_size,
    std::size_t price_bin_count,
    std::vector<double>& quantities)
{
    const double first_price = first_bin * price_bin_size;
    const double end_price = first_price
        + static_cast<double>(price_bin_count) * price_bin_size;

    for (
        auto level = levels.lower_bound(first_price);
        level != levels.end() && level->first < end_price;
        ++level
    )
    {
        const double price_bin = stable_floor(
            level->first / price_bin_size
        );
        const double offset = price_bin - first_bin;

        if (
            offset >= 0.0
            && offset < static_cast<double>(price_bin_count)
        )
        {
            quantities[static_cast<std::size_t>(offset)] +=
                level->second;
        }
    }
}
}

double automatic_price_bin_size(
    double midpoint,
    std::size_t price_bin_count)
{
    if (!std::isfinite(midpoint) || midpoint <= 0.0)
    {
        throw std::invalid_argument(
            "Automatic heatmap resolution requires a positive midpoint"
        );
    }

    if (price_bin_count == 0)
    {
        throw std::invalid_argument(
            "Automatic heatmap resolution requires price bins"
        );
    }

    const double raw_bin_size = midpoint
        * automatic_price_span_fraction
        / static_cast<double>(price_bin_count);
    const double magnitude = std::pow(
        10.0,
        std::floor(std::log10(raw_bin_size))
    );
    const double normalized = raw_bin_size / magnitude;
    constexpr double nice_factors[] = {1.0, 2.0, 2.5, 5.0, 10.0};
    double closest = nice_factors[0];

    for (const double candidate : nice_factors)
    {
        if (
            std::abs(candidate - normalized)
            < std::abs(closest - normalized)
        )
        {
            closest = candidate;
        }
    }

    const double resolved = closest * magnitude;

    if (!std::isfinite(resolved) || resolved <= 0.0)
    {
        throw std::invalid_argument(
            "Automatic heatmap price-bin size is outside supported range"
        );
    }

    return resolved;
}

double HeatmapColumn::price_at(std::size_t index) const
{
    if (index >= bid_quantities.size() || index >= ask_quantities.size())
    {
        throw std::out_of_range("Heatmap price-bin index is out of range");
    }

    return first_price + static_cast<double>(index) * price_bin_size;
}

HeatmapHistory::HeatmapHistory(HeatmapConfig config)
    : config_(config),
      resolved_price_bin_size_(config.price_bin_size)
{
    if (config_.time_bucket.count() <= 0)
    {
        throw std::invalid_argument("Heatmap time bucket must be positive");
    }

    if (
        config_.price_bin_size
        && (
            !std::isfinite(*config_.price_bin_size)
            || *config_.price_bin_size <= 0.0
        )
    )
    {
        throw std::invalid_argument("Heatmap price-bin size must be positive");
    }

    if (
        config_.price_bin_count == 0
        || config_.price_bin_count % 2 == 0
    )
    {
        throw std::invalid_argument(
            "Heatmap price-bin count must be positive and odd"
        );
    }

    if (config_.max_columns == 0)
    {
        throw std::invalid_argument("Heatmap history must retain a column");
    }
}

bool HeatmapHistory::sample(
    MarketTimestamp timestamp,
    const OrderBook& book)
{
    if (last_observation_ && timestamp < *last_observation_)
    {
        throw std::invalid_argument(
            "Heatmap observations must be chronological"
        );
    }

    last_observation_ = timestamp;

    if (!resolved_price_bin_size_)
    {
        const std::optional<OrderBook::Price> best_bid = book.best_bid();
        const std::optional<OrderBook::Price> best_ask = book.best_ask();

        if (!best_bid || !best_ask)
        {
            return false;
        }

        const double midpoint = *best_bid + (*best_ask - *best_bid) / 2.0;
        resolved_price_bin_size_ = automatic_price_bin_size(
            midpoint,
            config_.price_bin_count
        );
    }

    const MarketTimestamp column_timestamp = bucket_start(timestamp);
    std::optional<HeatmapColumn> column = make_column(
        column_timestamp,
        book
    );

    if (!column)
    {
        return false;
    }

    if (columns_.empty())
    {
        append(std::move(*column));
        carry_forward_ = true;
        return true;
    }

    if (column_timestamp == columns_.back().timestamp)
    {
        columns_.back() = std::move(*column);
        carry_forward_ = true;
        return true;
    }

    if (column_timestamp < columns_.back().timestamp)
    {
        throw std::invalid_argument(
            "Heatmap observations must be chronological"
        );
    }

    if (!carry_forward_)
    {
        append(std::move(*column));
        carry_forward_ = true;
        return true;
    }

    const std::int64_t elapsed_buckets =
        (column_timestamp - columns_.back().timestamp).count()
        / config_.time_bucket.count();

    if (
        static_cast<std::uint64_t>(elapsed_buckets)
        >= static_cast<std::uint64_t>(config_.max_columns)
    )
    {
        const HeatmapColumn carried = columns_.back();
        columns_.clear();

        for (
            std::size_t offset = config_.max_columns - 1;
            offset > 0;
            --offset
        )
        {
            HeatmapColumn gap_column = carried;
            gap_column.timestamp = column_timestamp
                - config_.time_bucket
                    * static_cast<std::int64_t>(offset);
            columns_.push_back(std::move(gap_column));
        }
    }
    else
    {
        HeatmapColumn carried = columns_.back();

        for (
            std::int64_t offset = 1;
            offset < elapsed_buckets;
            ++offset
        )
        {
            HeatmapColumn gap_column = carried;
            gap_column.timestamp += config_.time_bucket * offset;
            append(std::move(gap_column));
        }
    }

    append(std::move(*column));
    return true;
}

void HeatmapHistory::mark_discontinuity() noexcept
{
    carry_forward_ = false;
}

void HeatmapHistory::clear() noexcept
{
    columns_.clear();
    last_observation_.reset();
    carry_forward_ = true;
    resolved_price_bin_size_ = config_.price_bin_size;
}

const HeatmapConfig& HeatmapHistory::config() const noexcept
{
    return config_;
}

std::optional<double> HeatmapHistory::resolved_price_bin_size() const noexcept
{
    return resolved_price_bin_size_;
}

const std::deque<HeatmapColumn>& HeatmapHistory::columns() const noexcept
{
    return columns_;
}

std::optional<HeatmapColumn> HeatmapHistory::make_column(
    MarketTimestamp timestamp,
    const OrderBook& book) const
{
    const std::optional<OrderBook::Price> best_bid = book.best_bid();
    const std::optional<OrderBook::Price> best_ask = book.best_ask();

    if (!best_bid || !best_ask)
    {
        return std::nullopt;
    }

    const double mid_price = *best_bid + (*best_ask - *best_bid) / 2.0;

    if (!std::isfinite(mid_price))
    {
        throw std::invalid_argument("Heatmap midpoint must be finite");
    }

    if (!resolved_price_bin_size_)
    {
        throw std::logic_error("Heatmap price-bin size is unresolved");
    }

    const double price_bin_size = *resolved_price_bin_size_;

    const double center_bin = stable_floor(
        mid_price / price_bin_size
    );
    const double first_bin = center_bin
        - static_cast<double>(config_.price_bin_count / 2);

    HeatmapColumn column;
    column.timestamp = timestamp;
    column.first_price = first_bin * price_bin_size;
    column.price_bin_size = price_bin_size;
    column.mid_price = mid_price;
    column.bid_quantities.resize(config_.price_bin_count);
    column.ask_quantities.resize(config_.price_bin_count);

    accumulate_levels(
        book.bids(),
        first_bin,
        price_bin_size,
        config_.price_bin_count,
        column.bid_quantities
    );
    accumulate_levels(
        book.asks(),
        first_bin,
        price_bin_size,
        config_.price_bin_count,
        column.ask_quantities
    );

    return column;
}

MarketTimestamp HeatmapHistory::bucket_start(
    MarketTimestamp timestamp) const
{
    const std::int64_t timestamp_count = timestamp.time_since_epoch().count();
    const std::int64_t bucket_count = config_.time_bucket.count();
    std::int64_t quotient = timestamp_count / bucket_count;

    if (timestamp_count < 0 && timestamp_count % bucket_count != 0)
    {
        --quotient;
    }

    return MarketTimestamp{config_.time_bucket * quotient};
}

void HeatmapHistory::append(HeatmapColumn column)
{
    if (columns_.size() == config_.max_columns)
    {
        columns_.pop_front();
    }

    columns_.push_back(std::move(column));
}
