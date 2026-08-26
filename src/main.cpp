#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <fstream>
#include <iostream>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

int main()
{
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

        // Coinbase subscription
        std::string subscription = R"({
            "type": "subscribe",
            "channel": "level2",
            "product_ids": ["BTC-USD"]
        })";

        ws.write(asio::buffer(subscription));

        std::cout << "Subscribed to BTC-USD level2\n";

        // Open output file
        std::ofstream output_file("btc_usd.jsonl");

        if (!output_file.is_open())
        {
            throw std::runtime_error("Failed to open output file");
        }

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