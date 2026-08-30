#include "recorder_config.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace json = boost::json;

namespace
{
bool is_ascii_space(char character)
{
    return character == ' '
        || character == '\t'
        || character == '\n'
        || character == '\r'
        || character == '\f'
        || character == '\v';
}

bool is_ascii_alphanumeric(char character)
{
    return (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9');
}

std::string_view trim_ascii_space(std::string_view text)
{
    while (!text.empty() && is_ascii_space(text.front()))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && is_ascii_space(text.back()))
    {
        text.remove_suffix(1);
    }

    return text;
}
}

std::string normalize_product_id(std::string_view product_id)
{
    product_id = trim_ascii_space(product_id);

    if (product_id.empty() || product_id.size() > 65)
    {
        throw std::invalid_argument(
            "Product ID must be a BASE-QUOTE pair"
        );
    }

    std::string normalized(product_id);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char character)
        {
            if (character >= 'a' && character <= 'z')
            {
                return static_cast<char>(character - 'a' + 'A');
            }

            return static_cast<char>(character);
        }
    );

    const std::size_t separator = normalized.find('-');

    if (
        separator == std::string::npos
        || separator == 0
        || separator + 1 == normalized.size()
        || normalized.find('-', separator + 1) != std::string::npos
    )
    {
        throw std::invalid_argument(
            "Product ID must be a BASE-QUOTE pair"
        );
    }

    for (std::size_t index = 0; index < normalized.size(); ++index)
    {
        if (
            index != separator
            && !is_ascii_alphanumeric(normalized[index])
        )
        {
            throw std::invalid_argument(
                "Product ID may contain only letters, digits, and one hyphen"
            );
        }
    }

    return normalized;
}

std::string default_capture_path(std::string_view product_id)
{
    std::string path = normalize_product_id(product_id);

    std::transform(
        path.begin(),
        path.end(),
        path.begin(),
        [](unsigned char character)
        {
            if (character == '-')
            {
                return '_';
            }

            return static_cast<char>(std::tolower(character));
        }
    );

    return path + ".jsonl";
}

std::string make_level2_subscription(std::string_view product_id)
{
    json::array products;
    products.emplace_back(normalize_product_id(product_id));

    json::object subscription;
    subscription["type"] = "subscribe";
    subscription["channel"] = "level2";
    subscription["product_ids"] = std::move(products);
    return json::serialize(subscription);
}

RecorderOptions parse_recorder_options(
    std::span<const std::string_view> arguments)
{
    RecorderOptions options;
    bool product_set = false;
    bool output_set = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];

        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            return options;
        }

        if (argument == "--product")
        {
            if (product_set || index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "--product requires exactly one product ID"
                );
            }

            options.product_id = normalize_product_id(arguments[++index]);
            product_set = true;
            continue;
        }

        if (argument == "--output")
        {
            if (output_set || index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "--output requires exactly one path"
                );
            }

            options.output_path = arguments[++index];

            if (options.output_path.empty())
            {
                throw std::invalid_argument("Output path cannot be empty");
            }

            output_set = true;
            continue;
        }

        throw std::invalid_argument(
            "Unexpected recorder argument: " + std::string(argument)
        );
    }

    if (!output_set)
    {
        options.output_path = default_capture_path(options.product_id);
    }

    return options;
}
