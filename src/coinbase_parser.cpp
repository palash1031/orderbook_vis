#include "coinbase_parser.hpp"

#include <boost/json.hpp>

#include <stdexcept>
#include <string>
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
    const std::string text(json_string.c_str(), json_string.size());

    std::size_t processed_characters = 0;
    const double value = std::stod(text, &processed_characters);

    if (processed_characters != text.size())
    {
        throw std::invalid_argument(
            std::string("Invalid Coinbase Level 2 ")
            + field_name
            + ": "
            + text
        );
    }

    return value;
}
}

std::optional<ParsedBookMessage> CoinbaseParser::parse(
    const std::string& raw_message)
{
    try
    {
        const json::value parsed = json::parse(raw_message);
        const auto& object = parsed.as_object();
        const auto& channel = object.at("channel").as_string();

        if (channel != "l2_data")
        {
            return std::nullopt;
        }

        ParsedBookMessage message;
        message.sequence_num = parse_sequence_num(
            object.at("sequence_num")
        );

        const auto& events = object.at("events").as_array();

        if (events.empty())
        {
            throw std::invalid_argument(
                "Coinbase Level 2 message contains no events"
            );
        }

        std::optional<BookEventType> message_type;

        for (const auto& event_value : events)
        {
            const auto& event = event_value.as_object();
            const BookEventType event_type = parse_event_type(event);

            if (message_type && *message_type != event_type)
            {
                throw std::invalid_argument(
                    "Coinbase Level 2 message contains mixed event types"
                );
            }

            message_type = event_type;

            const auto& updates = event.at("updates").as_array();

            for (const auto& update_value : updates)
            {
                const auto& update_object = update_value.as_object();
                const auto& side_value =
                    update_object.at("side").as_string();

                BookUpdate update{
                    std::string(side_value.c_str(), side_value.size()),
                    parse_decimal_string(update_object, "price_level"),
                    parse_decimal_string(update_object, "new_quantity")
                };

                if (update.side != "bid" && update.side != "offer")
                {
                    throw std::invalid_argument(
                        "Unsupported Coinbase Level 2 side: "
                        + update.side
                    );
                }

                message.updates.push_back(std::move(update));
            }
        }

        message.type = *message_type;
        return message;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Failed to parse Coinbase message: ")
            + error.what()
        );
    }
}
