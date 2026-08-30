#pragma once

#include "book_types.hpp"
#include "order_book.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

struct HeatmapConfig
{
    std::chrono::nanoseconds time_bucket =
        std::chrono::milliseconds{100};
    // nullopt selects a bin size from the first usable midpoint and freezes it
    // for the current history session.
    std::optional<double> price_bin_size;
    std::size_t price_bin_count = 201;
    std::size_t max_columns = 600;
};

double automatic_price_bin_size(
    double midpoint,
    std::size_t price_bin_count
);

struct HeatmapColumn
{
    MarketTimestamp timestamp{};
    double first_price = 0.0;
    double price_bin_size = 0.0;
    double mid_price = 0.0;
    std::vector<double> bid_quantities;
    std::vector<double> ask_quantities;

    double price_at(std::size_t index) const;
};

class HeatmapHistory
{
public:
    explicit HeatmapHistory(HeatmapConfig config = {});

    // Samples the current book into its exchange-time bucket. Calls in the
    // same bucket replace the partial column, while skipped buckets carry
    // the last known state forward. Returns false until both book sides exist.
    bool sample(MarketTimestamp timestamp, const OrderBook& book);

    // Prevents the next valid sample from filling unknown time with stale
    // depth after a feed gap. The selected price resolution is preserved.
    void mark_discontinuity() noexcept;

    // Starts a fresh history session. Automatic resolution becomes unresolved
    // again; an explicitly configured resolution remains available.
    void clear() noexcept;

    const HeatmapConfig& config() const noexcept;
    std::optional<double> resolved_price_bin_size() const noexcept;
    const std::deque<HeatmapColumn>& columns() const noexcept;

private:
    std::optional<HeatmapColumn> make_column(
        MarketTimestamp timestamp,
        const OrderBook& book
    ) const;

    MarketTimestamp bucket_start(MarketTimestamp timestamp) const;
    void append(HeatmapColumn column);

    HeatmapConfig config_;
    std::optional<double> resolved_price_bin_size_;
    std::deque<HeatmapColumn> columns_;
    std::optional<MarketTimestamp> last_observation_;
    bool carry_forward_ = true;
};
