#include "replay_stream.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace json = boost::json;

namespace
{
std::int64_t parse_integer(const json::value& value, const char* field_name)
{
    if (value.is_int64())
    {
        return value.as_int64();
    }

    if (
        value.is_uint64()
        && value.as_uint64()
            <= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )
    )
    {
        return static_cast<std::int64_t>(value.as_uint64());
    }

    throw std::invalid_argument(
        std::string("Replay field must be an integer: ") + field_name
    );
}

double parse_non_negative_number(
    const json::value& value,
    const char* field_name)
{
    const double number = json::value_to<double>(value);

    if (!std::isfinite(number) || number < 0.0)
    {
        throw std::invalid_argument(
            std::string("Replay field must be finite and non-negative: ")
            + field_name
        );
    }

    return number;
}

const char* status_name(ReplayStatus status)
{
    switch (status)
    {
        case ReplayStatus::Paused:
            return "paused";
        case ReplayStatus::Playing:
            return "playing";
        case ReplayStatus::Complete:
            return "complete";
    }

    return "paused";
}

unsigned int parse_speed(const json::value& value)
{
    const std::int64_t speed = parse_integer(value, "speed");

    if (speed != 1 && speed != 5 && speed != 20)
    {
        throw std::invalid_argument("Replay speed must be 1, 5, or 20");
    }

    return static_cast<unsigned int>(speed);
}
}

ReplayStreamData parse_replay_stream(std::string_view document_json)
{
    try
    {
        const json::object document = json::parse(document_json).as_object();

        if (parse_integer(document.at("schema_version"), "schema_version") != 1)
        {
            throw std::invalid_argument("Unsupported heatmap schema version");
        }

        const auto& product = document.at("product_id").as_string();
        const json::object& config = document.at("config").as_object();
        const std::int64_t price_bin_count = parse_integer(
            config.at("price_bin_count"),
            "price_bin_count"
        );

        if (price_bin_count <= 0)
        {
            throw std::invalid_argument("Replay price-bin count must be positive");
        }

        const json::array& source_columns = document.at("columns").as_array();

        if (source_columns.empty())
        {
            throw std::invalid_argument("Replay contains no heatmap columns");
        }

        ReplayStreamData replay;
        replay.product_id.assign(product.c_str(), product.size());
        replay.columns.reserve(source_columns.size());

        std::int64_t previous_timestamp = 0;
        bool first_column = true;
        double peak_depth = 0.0;

        for (const json::value& value : source_columns)
        {
            const json::object& column = value.as_object();
            const std::int64_t timestamp = parse_integer(
                column.at("timestamp_ms"),
                "timestamp_ms"
            );

            if (!first_column && timestamp < previous_timestamp)
            {
                throw std::invalid_argument(
                    "Replay heatmap columns must be chronological"
                );
            }

            const json::array& bids = column.at("bids").as_array();
            const json::array& asks = column.at("asks").as_array();

            if (
                bids.size() != static_cast<std::size_t>(price_bin_count)
                || asks.size() != static_cast<std::size_t>(price_bin_count)
            )
            {
                throw std::invalid_argument(
                    "Replay column dimensions do not match price-bin count"
                );
            }

            parse_non_negative_number(column.at("first_price"), "first_price");
            parse_non_negative_number(column.at("mid_price"), "mid_price");

            for (std::size_t index = 0; index < bids.size(); ++index)
            {
                peak_depth = std::max(
                    peak_depth,
                    parse_non_negative_number(bids[index], "bid quantity")
                );
                peak_depth = std::max(
                    peak_depth,
                    parse_non_negative_number(asks[index], "ask quantity")
                );
            }

            replay.columns.push_back({timestamp, json::serialize(value)});
            previous_timestamp = timestamp;
            first_column = false;
        }

        json::object hello;
        hello["type"] = "hello";
        hello["schema_version"] = 1;
        hello["product_id"] = replay.product_id;
        hello["config"] = config;
        hello["total_columns"] = replay.columns.size();
        hello["start_timestamp_ms"] = replay.columns.front().timestamp_ms;
        hello["end_timestamp_ms"] = replay.columns.back().timestamp_ms;
        hello["peak_depth"] = peak_depth;
        replay.hello_message_json = json::serialize(hello);

        return replay;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Failed to load replay stream: ") + error.what()
        );
    }
}

ReplayPlayback::ReplayPlayback(const ReplayStreamData& replay)
    : replay_(replay)
{
    if (replay_.columns.empty())
    {
        throw std::invalid_argument("Replay playback requires heatmap columns");
    }
}

ReplayControlResult ReplayPlayback::apply_control(
    std::string_view command_json)
{
    try
    {
        const json::object command = json::parse(command_json).as_object();
        const auto& action_value = command.at("action").as_string();
        const std::string_view action(
            action_value.c_str(),
            action_value.size()
        );

        if (action == "play")
        {
            if (position_ == replay_.columns.size())
            {
                position_ = 0;
            }

            status_ = ReplayStatus::Playing;
            return ReplayControlResult::StateChanged;
        }

        if (action == "pause")
        {
            status_ = ReplayStatus::Paused;
            return ReplayControlResult::StateChanged;
        }

        if (action == "restart")
        {
            position_ = 0;
            status_ = ReplayStatus::Paused;
            return ReplayControlResult::Restarted;
        }

        if (action == "set_speed")
        {
            speed_ = parse_speed(command.at("speed"));
            return ReplayControlResult::StateChanged;
        }

        throw std::invalid_argument("Unsupported replay control action");
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Invalid replay control: ") + error.what()
        );
    }
}

std::optional<std::string> ReplayPlayback::take_next_column_message()
{
    if (
        status_ != ReplayStatus::Playing
        || position_ >= replay_.columns.size()
    )
    {
        return std::nullopt;
    }

    std::string message = R"({"type":"column","index":)"
        + std::to_string(position_)
        + R"(,"column":)"
        + replay_.columns[position_].payload_json
        + '}';
    ++position_;

    if (position_ == replay_.columns.size())
    {
        status_ = ReplayStatus::Complete;
    }

    return message;
}

std::string ReplayPlayback::state_message() const
{
    json::object message;
    message["type"] = "state";
    message["status"] = status_name(status_);
    message["speed"] = speed_;
    message["position"] = position_;
    message["total"] = replay_.columns.size();
    return json::serialize(message);
}

std::chrono::milliseconds ReplayPlayback::interval_to_next() const noexcept
{
    if (position_ == 0 || position_ >= replay_.columns.size())
    {
        return std::chrono::milliseconds{0};
    }

    const std::int64_t elapsed = std::max<std::int64_t>(
        0,
        replay_.columns[position_].timestamp_ms
            - replay_.columns[position_ - 1].timestamp_ms
    );
    const std::int64_t scaled = elapsed / speed_;

    if (elapsed > 0 && scaled == 0)
    {
        return std::chrono::milliseconds{1};
    }

    return std::chrono::milliseconds{scaled};
}

ReplayStatus ReplayPlayback::status() const noexcept
{
    return status_;
}

std::size_t ReplayPlayback::position() const noexcept
{
    return position_;
}

unsigned int ReplayPlayback::speed() const noexcept
{
    return speed_;
}
