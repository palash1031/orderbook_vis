#include "coinbase_parser.hpp"

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace json = boost::json;

namespace
{
BookEventType parse_event_type(const json::object& event)
{
    const auto& type = event.at("type").as_string();

    if (type == "snapshot")
    {
        return BookEventType::Snapshot;
    }

    if (type == "update")
    {
        return BookEventType::Update;
    }

    throw std::invalid_argument(
        "Unsupported Coinbase Level 2 event type: "
        + std::string(type.c_str(), type.size())
    );
}

BookSide parse_book_side(const json::object& update)
{
    const auto& side = update.at("side").as_string();

    if (side == "bid")
    {
        return BookSide::Bid;
    }

    if (side == "offer")
    {
        return BookSide::Offer;
    }

    throw std::invalid_argument(
        "Unsupported Coinbase Level 2 side: "
        + std::string(side.c_str(), side.size())
    );
}

int parse_timestamp_digits(
    std::string_view timestamp,
    std::size_t position,
    std::size_t count)
{
    int value = 0;

    for (std::size_t index = 0; index < count; ++index)
    {
        const char character = timestamp[position + index];

        if (character < '0' || character > '9')
        {
            throw std::invalid_argument("Invalid Coinbase timestamp");
        }

        value = value * 10 + (character - '0');
    }

    return value;
}

MarketTimestamp parse_timestamp(const json::value& value)
{
    const auto& json_string = value.as_string();
    const std::string_view timestamp(
        json_string.c_str(),
        json_string.size()
    );

    const bool whole_seconds =
        timestamp.size() == 20 && timestamp[19] == 'Z';
    const bool fractional_seconds =
        timestamp.size() >= 22
        && timestamp.size() <= 30
        && timestamp[19] == '.'
        && timestamp.back() == 'Z';

    if (
        timestamp.size() < 20
        || timestamp[4] != '-'
        || timestamp[7] != '-'
        || timestamp[10] != 'T'
        || timestamp[13] != ':'
        || timestamp[16] != ':'
        || (!whole_seconds && !fractional_seconds)
    )
    {
        throw std::invalid_argument("Invalid Coinbase timestamp");
    }

    const int year_value = parse_timestamp_digits(timestamp, 0, 4);
    const unsigned month_value = static_cast<unsigned>(
        parse_timestamp_digits(timestamp, 5, 2)
    );
    const unsigned day_value = static_cast<unsigned>(
        parse_timestamp_digits(timestamp, 8, 2)
    );
    const int hour_value = parse_timestamp_digits(timestamp, 11, 2);
    const int minute_value = parse_timestamp_digits(timestamp, 14, 2);
    const int second_value = parse_timestamp_digits(timestamp, 17, 2);

    const std::chrono::year_month_day date{
        std::chrono::year{year_value},
        std::chrono::month{month_value},
        std::chrono::day{day_value}
    };

    if (
        !date.ok()
        || hour_value > 23
        || minute_value > 59
        || second_value > 59
    )
    {
        throw std::invalid_argument("Invalid Coinbase timestamp");
    }

    std::int64_t fractional_nanoseconds = 0;

    if (fractional_seconds)
    {
        const std::size_t digit_count = timestamp.size() - 21;
        fractional_nanoseconds = parse_timestamp_digits(
            timestamp,
            20,
            digit_count
        );

        for (std::size_t index = digit_count; index < 9; ++index)
        {
            fractional_nanoseconds *= 10;
        }
    }

    return std::chrono::sys_days{date}
        + std::chrono::hours{hour_value}
        + std::chrono::minutes{minute_value}
        + std::chrono::seconds{second_value}
        + std::chrono::nanoseconds{fractional_nanoseconds};
}

std::uint64_t parse_sequence_num(const json::value& value)
{
    if (value.is_uint64())
    {
        return value.as_uint64();
    }

    if (value.is_int64())
    {
        const auto sequence_num = value.as_int64();

        if (sequence_num >= 0)
        {
            return static_cast<std::uint64_t>(sequence_num);
        }
    }

    throw std::invalid_argument(
        "Coinbase Level 2 sequence_num must be a non-negative integer"
    );
}

double parse_decimal_string(
    const json::object& update,
    const char* field_name)
{
    const auto& json_string = update.at(field_name).as_string();
    const char* const begin = json_string.c_str();
    char* end = nullptr;

    errno = 0;
    const double value = std::strtod(begin, &end);

    if (
        end == begin
        || end != begin + json_string.size()
        || errno == ERANGE
        || !std::isfinite(value)
    )
    {
        throw std::invalid_argument(
            std::string("Invalid Coinbase Level 2 ")
            + field_name
            + ": "
            + std::string(begin, json_string.size())
        );
    }

    return value;
}

ParsedCoinbaseFrame parse_frame_impl(const std::string& raw_message)
{
    const json::value parsed = json::parse(raw_message);
    const auto& object = parsed.as_object();
    const auto& channel = object.at("channel").as_string();
    const std::uint64_t sequence_num = parse_sequence_num(
        object.at("sequence_num")
    );

    ParsedCoinbaseFrame frame{sequence_num, {}};

    if (channel != "l2_data")
    {
        return frame;
    }

    const MarketTimestamp timestamp = parse_timestamp(
        object.at("timestamp")
    );
    const auto& events = object.at("events").as_array();

    if (events.empty())
    {
        throw std::invalid_argument(
            "Coinbase Level 2 message contains no events"
        );
    }

    frame.book_messages.reserve(events.size());

    for (const auto& event_value : events)
    {
        const auto& event = event_value.as_object();
        const BookEventType event_type = parse_event_type(event);
        const auto& event_product_id = event.at("product_id").as_string();
        const auto& updates = event.at("updates").as_array();

        ParsedBookMessage message{
            sequence_num,
            event_type,
            {},
            timestamp,
            std::string(
                event_product_id.c_str(),
                event_product_id.size()
            )
        };
        message.updates.reserve(updates.size());

        for (const auto& update_value : updates)
        {
            const auto& update_object = update_value.as_object();
            message.updates.emplace_back(
                parse_book_side(update_object),
                parse_decimal_string(update_object, "price_level"),
                parse_decimal_string(update_object, "new_quantity")
            );
        }

        frame.book_messages.push_back(std::move(message));
    }

    return frame;
}
}

ParsedCoinbaseFrame CoinbaseParser::parse_frame(
    const std::string& raw_message)
{
    try
    {
        return parse_frame_impl(raw_message);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Failed to parse Coinbase message: ")
            + error.what()
        );
    }
}

ParsedCoinbaseMessage CoinbaseParser::parse_message(
    const std::string& raw_message)
{
    try
    {
        ParsedCoinbaseFrame frame = parse_frame_impl(raw_message);

        if (frame.book_messages.empty())
        {
            return {frame.sequence_num, std::nullopt};
        }

        ParsedBookMessage message = std::move(frame.book_messages.front());

        for (
            auto iterator = std::next(frame.book_messages.begin());
            iterator != frame.book_messages.end();
            ++iterator
        )
        {
            if (message.type != iterator->type)
            {
                throw std::invalid_argument(
                    "Coinbase Level 2 message contains mixed event types"
                );
            }

            if (message.product_id != iterator->product_id)
            {
                throw std::invalid_argument(
                    "Coinbase Level 2 message contains mixed product IDs"
                );
            }

            message.updates.reserve(
                message.updates.size() + iterator->updates.size()
            );
            message.updates.insert(
                message.updates.end(),
                std::make_move_iterator(iterator->updates.begin()),
                std::make_move_iterator(iterator->updates.end())
            );
        }

        return {frame.sequence_num, std::move(message)};
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Failed to parse Coinbase message: ")
            + error.what()
        );
    }
}

std::optional<ParsedBookMessage> CoinbaseParser::parse(
    const std::string& raw_message)
{
    return parse_message(raw_message).book_message;
}
