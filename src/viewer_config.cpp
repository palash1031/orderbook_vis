#include "viewer_config.hpp"

#include "recorder_config.hpp"
#include "replay_config.hpp"

#include <boost/asio/ip/address.hpp>

#include <charconv>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
std::uint16_t parse_port(std::string_view text)
{
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (
        error != std::errc{}
        || end != text.data() + text.size()
        || value == 0
        || value > 65'535
    )
    {
        throw std::invalid_argument("Viewer port must be between 1 and 65535");
    }

    return static_cast<std::uint16_t>(value);
}

std::string parse_bind_address(std::string_view text)
{
    boost::system::error_code error;
    boost::asio::ip::make_address(text, error);

    if (error)
    {
        throw std::invalid_argument(
            "Viewer bind address must be a numeric IP address"
        );
    }

    return std::string(text);
}
}

ViewerOptions parse_viewer_options(
    std::span<const std::string_view> arguments,
    std::filesystem::path default_web_root,
    std::optional<std::string_view> environment_port)
{
    ViewerOptions options;
    options.web_root = std::move(default_web_root);
    bool heatmap_set = false;
    bool product_set = false;
    bool price_bin_set = false;
    bool venue_set = false;
    bool bind_set = false;
    bool port_set = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];

        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            return options;
        }

        if (argument == "--live")
        {
            if (options.live)
            {
                throw std::invalid_argument("--live may be specified once");
            }

            options.live = true;
            continue;
        }

        if (argument == "--scanner")
        {
            if (options.scanner)
            {
                throw std::invalid_argument(
                    "--scanner may be specified once"
                );
            }

            options.scanner = true;
            continue;
        }

        if (argument == "--public-demo")
        {
            if (options.public_demo)
            {
                throw std::invalid_argument(
                    "--public-demo may be specified once"
                );
            }

            options.public_demo = true;
            continue;
        }

        if (index + 1 >= arguments.size())
        {
            throw std::invalid_argument(
                "Missing value for " + std::string(argument)
            );
        }

        const std::string_view value = arguments[++index];

        if (argument == "--heatmap")
        {
            if (heatmap_set)
            {
                throw std::invalid_argument("--heatmap may be specified once");
            }

            options.heatmap_path = value;
            heatmap_set = true;
        }
        else if (argument == "--port")
        {
            if (port_set)
            {
                throw std::invalid_argument("--port may be specified once");
            }

            options.port = parse_port(value);
            port_set = true;
        }
        else if (argument == "--bind")
        {
            if (bind_set)
            {
                throw std::invalid_argument("--bind may be specified once");
            }

            options.bind_address = parse_bind_address(value);
            bind_set = true;
        }
        else if (argument == "--web-root")
        {
            options.web_root = value;
        }
        else if (argument == "--product")
        {
            if (product_set)
            {
                throw std::invalid_argument("--product may be specified once");
            }

            options.product_id = normalize_product_id(value);
            product_set = true;
        }
        else if (argument == "--price-bin")
        {
            if (price_bin_set)
            {
                throw std::invalid_argument(
                    "--price-bin may be specified once"
                );
            }

            options.live_heatmap_config.price_bin_size =
                parse_price_bin_argument(value);
            price_bin_set = true;
        }
        else if (argument == "--venue")
        {
            if (venue_set)
            {
                throw std::invalid_argument("--venue may be specified once");
            }

            options.venue = parse_venue(value);
            venue_set = true;
        }
        else
        {
            throw std::invalid_argument(
                "Unknown viewer option: " + std::string(argument)
            );
        }
    }

    if (!port_set && environment_port)
    {
        options.port = parse_port(*environment_port);
    }

    if (options.live && heatmap_set)
    {
        throw std::invalid_argument(
            "--live and --heatmap select different stream sources"
        );
    }

    if (options.scanner && options.live)
    {
        throw std::invalid_argument(
            "--scanner and --live select different stream sources"
        );
    }

    if (options.scanner && heatmap_set)
    {
        throw std::invalid_argument(
            "--scanner and --heatmap select different stream sources"
        );
    }

    if (options.scanner && (product_set || price_bin_set || venue_set))
    {
        throw std::invalid_argument(
            "--product, --price-bin, and --venue are not scanner options"
        );
    }

    if (
        !options.live
        && !options.scanner
        && (product_set || price_bin_set || venue_set)
    )
    {
        throw std::invalid_argument(
            "--product, --price-bin, and --venue require --live"
        );
    }

    return options;
}
