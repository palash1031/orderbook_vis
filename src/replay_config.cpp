#include "replay_config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

std::optional<double> parse_price_bin_argument(std::string_view value)
{
    if (value == "auto")
    {
        return std::nullopt;
    }

    const std::string text(value);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);

    if (
        value.empty()
        || end != text.c_str() + text.size()
        || errno == ERANGE
        || !std::isfinite(parsed)
        || parsed <= 0.0
    )
    {
        throw std::invalid_argument(
            "--price-bin must be 'auto' or a positive finite number"
        );
    }

    return parsed;
}

ReplayOptions parse_replay_options(
    std::span<const std::string_view> arguments)
{
    ReplayOptions options;
    bool input_path_set = false;
    bool price_bin_set = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];

        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            return options;
        }

        if (argument == "--heatmap-output")
        {
            if (index + 1 >= arguments.size() || options.heatmap_output_path)
            {
                throw std::invalid_argument(
                    "--heatmap-output requires exactly one path"
                );
            }

            options.heatmap_output_path = arguments[++index];

            if (options.heatmap_output_path->empty())
            {
                throw std::invalid_argument(
                    "Heatmap output path cannot be empty"
                );
            }

            continue;
        }

        if (argument == "--price-bin")
        {
            if (price_bin_set || index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "--price-bin requires exactly one value"
                );
            }

            options.heatmap_config.price_bin_size =
                parse_price_bin_argument(arguments[++index]);
            price_bin_set = true;
            continue;
        }

        if (argument.starts_with('-') || input_path_set)
        {
            throw std::invalid_argument(
                "Unexpected replay argument: " + std::string(argument)
            );
        }

        options.input_path = argument;
        input_path_set = true;
    }

    return options;
}
