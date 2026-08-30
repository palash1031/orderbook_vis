#include "recorder_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

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
        asio::io_context ioc;

        tcp::resolver resolver{ioc};

        std::string host = "advanced-trade-ws.coinbase.com";
        std::string port = "443";

        // DNS resolution
        auto results = resolver.resolve(host, port);

        for (const auto& result : results)
        {
            std::cout << result.endpoint() << '\n';
        }

        // TLS configuration
        ssl::context ssl_ctx{ssl::context::tls_client};
        ssl_ctx.set_default_verify_paths();

        // WebSocket -> TLS -> TCP
        websocket::stream<
            beast::ssl_stream<
                beast::tcp_stream
            >
        > ws{ioc, ssl_ctx};

        // TCP connect
        beast::get_lowest_layer(ws).connect(results);

        std::cout << "TCP connected\n";

        // SNI hostname for TLS
        if (!SSL_set_tlsext_host_name(
                ws.next_layer().native_handle(),
                host.c_str()))
        {
            throw std::runtime_error("Failed to set SNI hostname");
        }

        // TLS certificate verification
        ws.next_layer().set_verify_mode(ssl::verify_peer);

        ws.next_layer().set_verify_callback(
            ssl::host_name_verification(host)
        );

        // TLS handshake
        ws.next_layer().handshake(
            ssl::stream_base::client
        );

        std::cout << "TLS connected\n";

        // WebSocket handshake
        ws.handshake(host, "/");

        std::cout << "WebSocket connected\n";

        const std::string subscription = make_level2_subscription(
            options.product_id
        );

        ws.write(asio::buffer(subscription));

        std::cout
            << "Subscription requested for "
            << options.product_id
            << " level2\n";

        // Open output file
        std::ofstream output_file(options.output_path);

        if (!output_file.is_open())
        {
            throw std::runtime_error(
                "Failed to open " + options.output_path
            );
        }

        std::cout << "Recording to " << options.output_path << '\n';

        // Buffer for incoming WebSocket messages
        beast::flat_buffer buffer;

        while (true)
        {
            ws.read(buffer);

            // Convert current message to a std::string
            std::string message =
                beast::buffers_to_string(buffer.data());

            // Print to terminal
            std::cout << message << '\n';

            // Write one WebSocket message per line
            output_file << message << '\n';

            // Clear the buffer for the next message
            buffer.consume(buffer.size());
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
