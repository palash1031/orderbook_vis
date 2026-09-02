#include "recorder_config.hpp"

#include "market.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace json = boost::json;

std::string normalize_product_id(std::string_view product_id)
{
    return Product::parse(product_id).to_string();
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

std::string make_heartbeat_subscription()
{
    json::object subscription;
    subscription["type"] = "subscribe";
    subscription["channel"] = "heartbeats";
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
