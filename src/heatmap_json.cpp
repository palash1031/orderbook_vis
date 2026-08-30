#include "heatmap_json.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void write_quantities(
    std::ostream& output,
    const std::vector<double>& quantities)
{
    output << '[';

    for (std::size_t index = 0; index < quantities.size(); ++index)
    {
        if (index != 0)
        {
            output << ',';
        }

        output << quantities[index];
    }

    output << ']';
}
}

void write_heatmap_json(
    std::ostream& output,
    std::string_view product_id,
    const HeatmapHistory& heatmap)
{
    const HeatmapConfig& config = heatmap.config();
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output
        << R"({"schema_version":1,"product_id":)"
        << boost::json::serialize(std::string(product_id))
        << R"(,"config":{"time_bucket_ns":)"
        << config.time_bucket.count()
        << R"(,"price_bin_size":)"
        << config.price_bin_size
        << R"(,"price_bin_count":)"
        << config.price_bin_count
        << R"(,"max_columns":)"
        << config.max_columns
        << R"(},"columns":[)";

    const auto& columns = heatmap.columns();

    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        if (index != 0)
        {
            output << ',';
        }

        const HeatmapColumn& column = columns[index];
        const auto timestamp_nanoseconds =
            column.timestamp.time_since_epoch().count();
        const auto timestamp_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                column.timestamp.time_since_epoch()
            ).count();

        output
            << R"({"timestamp_ns":")"
            << timestamp_nanoseconds
            << R"(","timestamp_ms":)"
            << timestamp_milliseconds
            << R"(,"first_price":)"
            << column.first_price
            << R"(,"mid_price":)"
            << column.mid_price
            << R"(,"bids":)";
        write_quantities(output, column.bid_quantities);
        output << R"(,"asks":)";
        write_quantities(output, column.ask_quantities);
        output << '}';
    }

    output << "]}";

    if (!output)
    {
        throw std::runtime_error("Failed to write heatmap JSON");
    }
}
