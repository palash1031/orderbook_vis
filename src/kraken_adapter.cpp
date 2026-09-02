#include "kraken_adapter.hpp"

#include <boost/json.hpp>
#include <boost/crc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
std::string json_string(const json::object& object, const char* field)
{
    const auto& value = object.at(field).as_string();
    return std::string(value.c_str(), value.size());
}

bool valid_native_symbol(std::string_view symbol) noexcept
{
    const std::size_t separator = symbol.find('/');
    return separator != std::string_view::npos
        && separator != 0
        && separator + 1 != symbol.size()
        && symbol.find('/', separator + 1) == std::string_view::npos;
}

int timestamp_digits(
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
            throw std::invalid_argument("Invalid Kraken timestamp");
        }

        value = value * 10 + character - '0';
    }

    return value;
}

MarketTimestamp parse_timestamp(const json::value& value)
{
    const auto& text = value.as_string();
    const std::string_view timestamp(text.c_str(), text.size());
    const bool whole = timestamp.size() == 20 && timestamp[19] == 'Z';
    const bool fractional = timestamp.size() >= 22
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
        || (!whole && !fractional)
    )
    {
        throw std::invalid_argument("Invalid Kraken timestamp");
    }

    const std::chrono::year_month_day date{
        std::chrono::year{timestamp_digits(timestamp, 0, 4)},
        std::chrono::month{static_cast<unsigned>(
            timestamp_digits(timestamp, 5, 2)
        )},
        std::chrono::day{static_cast<unsigned>(
            timestamp_digits(timestamp, 8, 2)
        )}
    };
    const int hour = timestamp_digits(timestamp, 11, 2);
    const int minute = timestamp_digits(timestamp, 14, 2);
    const int second = timestamp_digits(timestamp, 17, 2);

    if (!date.ok() || hour > 23 || minute > 59 || second > 59)
    {
        throw std::invalid_argument("Invalid Kraken timestamp");
    }

    std::int64_t nanoseconds = 0;

    if (fractional)
    {
        const std::size_t digits = timestamp.size() - 21;
        nanoseconds = timestamp_digits(timestamp, 20, digits);

        for (std::size_t index = digits; index < 9; ++index)
        {
            nanoseconds *= 10;
        }
    }

    return std::chrono::sys_days{date}
        + std::chrono::hours{hour}
        + std::chrono::minutes{minute}
        + std::chrono::seconds{second}
        + std::chrono::nanoseconds{nanoseconds};
}

struct ParsedDecimal
{
    std::string text;
    bool zero;
};

ParsedDecimal parse_decimal(std::string text, bool allow_zero)
{
    if (text.empty())
    {
        throw std::invalid_argument("Empty Kraken book decimal");
    }

    bool saw_digit = false;
    bool saw_point = false;
    bool saw_nonzero_digit = false;

    for (const char character : text)
    {
        if (character >= '0' && character <= '9')
        {
            saw_digit = true;
            saw_nonzero_digit = saw_nonzero_digit || character != '0';
        }
        else if (character == '.' && !saw_point)
        {
            saw_point = true;
        }
        else
        {
            throw std::invalid_argument("Malformed Kraken book decimal");
        }
    }

    if (!saw_digit || text.front() == '.' || text.back() == '.')
    {
        throw std::invalid_argument("Malformed Kraken book decimal");
    }

    if (!allow_zero && !saw_nonzero_digit)
    {
        throw std::invalid_argument("Invalid Kraken book decimal");
    }

    return {std::move(text), !saw_nonzero_digit};
}

double visualization_decimal(std::string_view text)
{
    double value = 0.0;

    try
    {
        value = json::value_to<double>(json::parse(text));
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument(
            "Kraken book decimal cannot be visualized"
        );
    }

    if (!std::isfinite(value))
    {
        throw std::invalid_argument(
            "Kraken book decimal cannot be visualized"
        );
    }

    return value;
}

std::string canonical_decimal(std::string_view decimal)
{
    const std::size_t point = decimal.find('.');
    std::string_view integer = decimal.substr(0, point);
    std::string_view fraction = point == std::string_view::npos
        ? std::string_view{}
        : decimal.substr(point + 1);
    const std::size_t first_integer = integer.find_first_not_of('0');

    if (first_integer == std::string_view::npos)
    {
        integer = "0";
    }
    else
    {
        integer.remove_prefix(first_integer);
    }

    const std::size_t last_fraction = fraction.find_last_not_of('0');

    if (last_fraction == std::string_view::npos)
    {
        fraction = {};
    }
    else
    {
        fraction = fraction.substr(0, last_fraction + 1);
    }

    std::string canonical(integer);

    if (!fraction.empty())
    {
        canonical += '.';
        canonical += fraction;
    }

    return canonical;
}

bool exact_decimal_less(
    std::string_view left,
    std::string_view right) noexcept
{
    const std::size_t left_point = left.find('.');
    const std::size_t right_point = right.find('.');
    const std::string_view left_integer = left.substr(0, left_point);
    const std::string_view right_integer = right.substr(0, right_point);

    if (left_integer.size() != right_integer.size())
    {
        return left_integer.size() < right_integer.size();
    }

    if (left_integer != right_integer)
    {
        return left_integer < right_integer;
    }

    const std::string_view left_fraction =
        left_point == std::string_view::npos
        ? std::string_view{}
        : left.substr(left_point + 1);
    const std::string_view right_fraction =
        right_point == std::string_view::npos
        ? std::string_view{}
        : right.substr(right_point + 1);
    const std::size_t digits = std::max(
        left_fraction.size(),
        right_fraction.size()
    );

    for (std::size_t index = 0; index < digits; ++index)
    {
        const char left_digit = index < left_fraction.size()
            ? left_fraction[index]
            : '0';
        const char right_digit = index < right_fraction.size()
            ? right_fraction[index]
            : '0';

        if (left_digit != right_digit)
        {
            return left_digit < right_digit;
        }
    }

    return false;
}

void skip_space(std::string_view text, std::size_t& position) noexcept
{
    while (
        position < text.size()
        && std::isspace(static_cast<unsigned char>(text[position]))
    )
    {
        ++position;
    }
}

std::size_t string_end(std::string_view text, std::size_t position)
{
    if (position >= text.size() || text[position] != '"')
    {
        throw std::invalid_argument("Expected Kraken JSON string");
    }

    bool escaped = false;

    for (++position; position < text.size(); ++position)
    {
        const char character = text[position];

        if (escaped)
        {
            escaped = false;
        }
        else if (character == '\\')
        {
            escaped = true;
        }
        else if (character == '"')
        {
            return position + 1;
        }
    }

    throw std::invalid_argument("Unterminated Kraken JSON string");
}

std::size_t balanced_end(std::string_view text, std::size_t position)
{
    if (
        position >= text.size()
        || (text[position] != '[' && text[position] != '{')
    )
    {
        throw std::invalid_argument("Expected Kraken JSON container");
    }

    std::vector<char> expected;
    expected.push_back(text[position] == '[' ? ']' : '}');

    for (++position; position < text.size(); ++position)
    {
        const char character = text[position];

        if (character == '"')
        {
            position = string_end(text, position) - 1;
            continue;
        }

        if (character == '[' || character == '{')
        {
            expected.push_back(character == '[' ? ']' : '}');
        }
        else if (character == ']' || character == '}')
        {
            if (expected.empty() || expected.back() != character)
            {
                throw std::invalid_argument("Malformed Kraken JSON container");
            }

            expected.pop_back();

            if (expected.empty())
            {
                return position + 1;
            }
        }
    }

    throw std::invalid_argument("Unterminated Kraken JSON container");
}

std::string_view find_member_array(
    std::string_view message,
    std::string_view field)
{
    for (std::size_t position = 0; position < message.size();)
    {
        if (message[position] != '"')
        {
            ++position;
            continue;
        }

        const std::size_t end = string_end(message, position);
        const std::string_view key = message.substr(
            position + 1,
            end - position - 2
        );
        std::size_t value = end;
        skip_space(message, value);

        if (key == field && value < message.size() && message[value] == ':')
        {
            ++value;
            skip_space(message, value);

            if (value < message.size() && message[value] == '[')
            {
                const std::size_t array_end = balanced_end(message, value);
                return message.substr(value, array_end - value);
            }
        }

        position = end;
    }

    throw std::invalid_argument("Missing Kraken book level array");
}

std::vector<std::string_view> split_object_array(std::string_view array)
{
    if (array.size() < 2 || array.front() != '[' || array.back() != ']')
    {
        throw std::invalid_argument("Malformed Kraken book level array");
    }

    std::vector<std::string_view> objects;
    std::size_t position = 1;

    while (true)
    {
        skip_space(array, position);

        if (position >= array.size() - 1)
        {
            return objects;
        }

        if (array[position] != '{')
        {
            throw std::invalid_argument("Kraken book level must be an object");
        }

        const std::size_t end = balanced_end(array, position);
        objects.push_back(array.substr(position, end - position));
        position = end;
        skip_space(array, position);

        if (position < array.size() && array[position] == ',')
        {
            ++position;
            continue;
        }

        if (position == array.size() - 1)
        {
            return objects;
        }

        throw std::invalid_argument("Malformed Kraken book level array");
    }
}

std::string decimal_member(
    std::string_view object,
    std::string_view requested)
{
    if (object.size() < 2 || object.front() != '{' || object.back() != '}')
    {
        throw std::invalid_argument("Malformed Kraken book level");
    }

    std::size_t position = 1;

    while (position < object.size() - 1)
    {
        skip_space(object, position);

        if (position == object.size() - 1)
        {
            break;
        }

        if (object[position] != '"')
        {
            throw std::invalid_argument("Malformed Kraken book level member");
        }

        const std::size_t key_end = string_end(object, position);
        const std::string_view key = object.substr(
            position + 1,
            key_end - position - 2
        );
        position = key_end;
        skip_space(object, position);

        if (position >= object.size() || object[position++] != ':')
        {
            throw std::invalid_argument("Malformed Kraken book level member");
        }

        skip_space(object, position);

        if (position >= object.size())
        {
            throw std::invalid_argument("Missing Kraken book level value");
        }

        const std::size_t value_begin = position;
        std::size_t value_end = position;

        if (object[position] == '"')
        {
            value_end = string_end(object, position);
        }
        else if (object[position] == '[' || object[position] == '{')
        {
            value_end = balanced_end(object, position);
        }
        else
        {
            while (
                value_end < object.size()
                && object[value_end] != ','
                && object[value_end] != '}'
                && !std::isspace(static_cast<unsigned char>(object[value_end]))
            )
            {
                ++value_end;
            }
        }

        if (key == requested)
        {
            if (object[value_begin] == '"')
            {
                const std::string_view quoted = object.substr(
                    value_begin + 1,
                    value_end - value_begin - 2
                );

                if (quoted.find('\\') != std::string_view::npos)
                {
                    throw std::invalid_argument(
                        "Escaped Kraken book decimals are unsupported"
                    );
                }

                return std::string(quoted);
            }

            if (object[value_begin] == '[' || object[value_begin] == '{')
            {
                throw std::invalid_argument("Invalid Kraken book decimal");
            }

            return std::string(object.substr(
                value_begin,
                value_end - value_begin
            ));
        }

        position = value_end;
        skip_space(object, position);

        if (position < object.size() && object[position] == ',')
        {
            ++position;
        }
    }

    throw std::invalid_argument("Incomplete Kraken book level");
}

std::uint32_t parse_checksum(const json::value& value)
{
    std::uint64_t checksum = 0;

    if (value.is_uint64())
    {
        checksum = value.as_uint64();
    }
    else if (value.is_int64() && value.as_int64() >= 0)
    {
        checksum = static_cast<std::uint64_t>(value.as_int64());
    }
    else
    {
        throw std::invalid_argument("Invalid Kraken checksum");
    }

    if (checksum > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument("Invalid Kraken checksum");
    }

    return static_cast<std::uint32_t>(checksum);
}

std::string checksum_decimal(std::string_view decimal)
{
    std::string result;
    result.reserve(decimal.size());

    for (const char character : decimal)
    {
        if (character != '.')
        {
            result.push_back(character);
        }
    }

    const std::size_t first = result.find_first_not_of('0');
    return first == std::string::npos ? "0" : result.substr(first);
}
}

bool is_supported_kraken_book_depth(std::size_t depth) noexcept
{
    return depth == 10
        || depth == 25
        || depth == 100
        || depth == 500
        || depth == 1000;
}

std::string make_kraken_instrument_subscription()
{
    json::object params;
    params["channel"] = "instrument";
    params["snapshot"] = true;

    json::object request;
    request["method"] = "subscribe";
    request["params"] = std::move(params);
    return json::serialize(request);
}

std::string make_kraken_book_subscription(
    std::string_view native_symbol,
    std::size_t depth)
{
    if (!valid_native_symbol(native_symbol))
    {
        throw std::invalid_argument("Kraken book requires a native symbol");
    }

    if (!is_supported_kraken_book_depth(depth))
    {
        throw std::invalid_argument(
            "Kraken depth must be one of 10, 25, 100, 500, or 1000"
        );
    }

    json::array symbols;
    symbols.emplace_back(native_symbol);

    json::object params;
    params["channel"] = "book";
    params["symbol"] = std::move(symbols);
    params["depth"] = static_cast<std::int64_t>(depth);
    params["snapshot"] = true;

    json::object request;
    request["method"] = "subscribe";
    request["params"] = std::move(params);
    return json::serialize(request);
}

KrakenInstrumentCatalog KrakenInstrumentCatalog::parse(
    std::string_view raw_message)
{
    try
    {
        const json::object message = json::parse(raw_message).as_object();

        if (
            message.at("channel").as_string() != "instrument"
            || message.at("type").as_string() != "snapshot"
        )
        {
            throw std::invalid_argument(
                "Expected a Kraken instrument snapshot"
            );
        }

        const json::array& pairs = message.at("data")
            .as_object().at("pairs").as_array();
        KrakenInstrumentCatalog catalog;

        for (const json::value& pair_value : pairs)
        {
            const json::object& pair = pair_value.as_object();
            const std::string symbol = json_string(pair, "symbol");
            const Product product(
                json_string(pair, "base"),
                json_string(pair, "quote")
            );
            const std::string status = json_string(pair, "status");

            if (!valid_native_symbol(symbol))
            {
                throw std::invalid_argument("Invalid Kraken native symbol");
            }

            if (status != "online")
            {
                continue;
            }

            if (
                !catalog.native_by_product_.emplace(product, symbol).second
                || !catalog.product_by_native_.emplace(symbol, product).second
            )
            {
                throw std::invalid_argument(
                    "Ambiguous Kraken instrument metadata"
                );
            }
        }

        return catalog;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Failed to parse Kraken instrument metadata: ")
            + error.what()
        );
    }
}

std::optional<std::string> KrakenInstrumentCatalog::native_symbol(
    const Product& product) const
{
    const auto found = native_by_product_.find(product);
    return found == native_by_product_.end()
        ? std::nullopt
        : std::optional<std::string>{found->second};
}

std::optional<Product> KrakenInstrumentCatalog::canonical_product(
    std::string_view native_symbol) const
{
    const auto found = product_by_native_.find(std::string(native_symbol));
    return found == product_by_native_.end()
        ? std::nullopt
        : std::optional<Product>{found->second};
}

bool KrakenBookAdapter::ExactDecimalLess::operator()(
    const std::string& left,
    const std::string& right) const noexcept
{
    return exact_decimal_less(left, right);
}

KrakenBookAdapter::KrakenBookAdapter(
    Product product,
    std::string_view native_symbol,
    std::size_t depth)
    : product_(std::move(product)),
      native_symbol_(native_symbol),
      depth_(depth)
{
    if (!valid_native_symbol(native_symbol_))
    {
        throw std::invalid_argument("Invalid Kraken native symbol");
    }

    if (!is_supported_kraken_book_depth(depth_))
    {
        throw std::invalid_argument("Unsupported Kraken book depth");
    }
}

std::optional<TrustedBookEvent> KrakenBookAdapter::process(
    std::string_view raw_message)
{
    try
    {
        const json::object message = json::parse(raw_message).as_object();
        const auto* channel = message.if_contains("channel");

        if (!channel || !channel->is_string() || channel->as_string() != "book")
        {
            return std::nullopt;
        }

        const auto& type = message.at("type").as_string();

        if (type != "snapshot" && type != "update")
        {
            throw std::invalid_argument("Unsupported Kraken book event type");
        }

        if (type == "update" && status_ != VenueMarketStatus::Live)
        {
            return std::nullopt;
        }

        const json::array& data = message.at("data").as_array();

        if (data.size() != 1)
        {
            throw std::invalid_argument(
                "Kraken book message must contain exactly one product"
            );
        }

        const json::object& payload = data.front().as_object();

        if (json_string(payload, "symbol") != native_symbol_)
        {
            throw std::invalid_argument("Unexpected Kraken book symbol");
        }

        Levels next_bids = type == "snapshot" ? Levels{} : bids_;
        Levels next_asks = type == "snapshot" ? Levels{} : asks_;

        const auto apply = [](
            const json::array& parsed_updates,
            std::string_view raw_array,
            Levels& levels,
            bool allow_deletion)
        {
            const std::vector<std::string_view> raw_updates =
                split_object_array(raw_array);

            if (raw_updates.size() != parsed_updates.size())
            {
                throw std::invalid_argument("Incomplete Kraken book level");
            }

            for (std::size_t index = 0; index < raw_updates.size(); ++index)
            {
                const json::object& parsed = parsed_updates[index].as_object();
                parsed.at("price");
                parsed.at("qty");
                ParsedDecimal price = parse_decimal(
                    decimal_member(raw_updates[index], "price"),
                    false
                );
                ParsedDecimal quantity = parse_decimal(
                    decimal_member(raw_updates[index], "qty"),
                    true
                );
                const std::string price_key = canonical_decimal(price.text);

                if (quantity.zero)
                {
                    if (!allow_deletion)
                    {
                        throw std::invalid_argument(
                            "Kraken snapshot quantity must be positive"
                        );
                    }

                    levels.erase(price_key);
                }
                else
                {
                    levels[price_key] = {
                        std::move(price.text),
                        std::move(quantity.text)
                    };
                }
            }
        };

        apply(
            payload.at("asks").as_array(),
            find_member_array(raw_message, "asks"),
            next_asks,
            type == "update"
        );
        apply(
            payload.at("bids").as_array(),
            find_member_array(raw_message, "bids"),
            next_bids,
            type == "update"
        );

        if (type == "snapshot" && (next_asks.empty() || next_bids.empty()))
        {
            throw std::invalid_argument(
                "Kraken book snapshot must contain both sides"
            );
        }

        truncate(next_bids, next_asks, depth_);
        const std::string input = checksum_input_for(next_bids, next_asks);
        boost::crc_32_type crc;
        crc.process_bytes(input.data(), input.size());

        if (crc.checksum() != parse_checksum(payload.at("checksum")))
        {
            return invalidation();
        }

        const MarketTimestamp timestamp = parse_timestamp(
            payload.at("timestamp")
        );
        OrderBook book = numeric_book_for(next_bids, next_asks);
        bids_ = std::move(next_bids);
        asks_ = std::move(next_asks);
        last_timestamp_ = timestamp;
        status_ = VenueMarketStatus::Live;
        return TrustedBookEvent{
            type == "snapshot"
                ? TrustedBookEventType::Snapshot
                : TrustedBookEventType::Update,
            {Venue::Kraken, product_},
            timestamp,
            std::move(book)
        };
    }
    catch (const std::exception&)
    {
        return invalidation();
    }
}

void KrakenBookAdapter::reset() noexcept
{
    bids_.clear();
    asks_.clear();
    status_ = VenueMarketStatus::WaitingForSnapshot;
    last_timestamp_ = {};
}

VenueMarketStatus KrakenBookAdapter::status() const noexcept
{
    return status_;
}

std::uint32_t KrakenBookAdapter::checksum() const
{
    const std::string input = checksum_input();
    boost::crc_32_type crc;
    crc.process_bytes(input.data(), input.size());
    return crc.checksum();
}

std::string KrakenBookAdapter::checksum_input() const
{
    return checksum_input_for(bids_, asks_);
}

std::string KrakenBookAdapter::checksum_input_for(
    const Levels& bids,
    const Levels& asks)
{
    std::string input;
    std::size_t count = 0;

    for (auto level = asks.begin(); level != asks.end() && count < 10;
         ++level, ++count)
    {
        input += checksum_decimal(level->second.price_text);
        input += checksum_decimal(level->second.quantity_text);
    }

    count = 0;

    for (auto level = bids.rbegin(); level != bids.rend() && count < 10;
         ++level, ++count)
    {
        input += checksum_decimal(level->second.price_text);
        input += checksum_decimal(level->second.quantity_text);
    }

    return input;
}

void KrakenBookAdapter::truncate(
    Levels& bids,
    Levels& asks,
    std::size_t depth)
{
    while (asks.size() > depth)
    {
        asks.erase(std::prev(asks.end()));
    }

    while (bids.size() > depth)
    {
        bids.erase(bids.begin());
    }
}

TrustedBookEvent KrakenBookAdapter::invalidation() noexcept
{
    bids_.clear();
    asks_.clear();
    status_ = VenueMarketStatus::Stale;
    return {
        TrustedBookEventType::Invalidated,
        {Venue::Kraken, product_},
        last_timestamp_,
        std::nullopt
    };
}

OrderBook KrakenBookAdapter::numeric_book_for(
    const Levels& bids,
    const Levels& asks)
{
    OrderBook book;

    for (const auto& [price, level] : bids)
    {
        static_cast<void>(price);
        book.apply_update(
            BookSide::Bid,
            visualization_decimal(level.price_text),
            visualization_decimal(level.quantity_text)
        );
    }

    for (const auto& [price, level] : asks)
    {
        static_cast<void>(price);
        book.apply_update(
            BookSide::Offer,
            visualization_decimal(level.price_text),
            visualization_decimal(level.quantity_text)
        );
    }

    return book;
}
