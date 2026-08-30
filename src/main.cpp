#include "recorder_config.hpp"
#include "coinbase_level2_stream.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void print_usage(const char* executable)
{
    std::cout
        << "Usage: "
        << executable
        << " [--product BTC-USD] [--output btc_usd.jsonl]\n";
}
}

int main(int argc, char* argv[])
{
    RecorderOptions options;

    try
    {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 1));

        for (int index = 1; index < argc; ++index)
        {
            arguments.emplace_back(argv[index]);
        }

        options = parse_recorder_options(arguments);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }

    if (options.show_help)
    {
        print_usage(argv[0]);
        return 0;
    }

    try
    {
        std::ofstream output_file(options.output_path);

        if (!output_file.is_open())
        {
            throw std::runtime_error(
                "Failed to open " + options.output_path
            );
        }

        std::cout << "Recording to " << options.output_path << '\n';
        CoinbaseLevel2Stream stream(options.product_id);
        std::cout
            << "Connected; subscription requested for "
            << options.product_id
            << " level2\n";

        while (true)
        {
            const std::string message = stream.read();

            // Print to terminal
            std::cout << message << '\n';

            // Write one WebSocket message per line
            output_file << message << '\n';

        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
