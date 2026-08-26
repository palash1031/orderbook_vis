#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

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

        auto results = resolver.resolve(host, port);

        for (const auto& result : results)
        {
            std::cout << result.endpoint() << '\n';
        }

        ssl::context ssl_ctx{ssl::context::tls_client};

        ssl_ctx.set_default_verify_paths();

        websocket::stream<
            beast::ssl_stream<
                beast::tcp_stream
            >
        > ws{ioc, ssl_ctx};

        // TCP connection
        beast::get_lowest_layer(ws).connect(results);

        std::cout << "TCP connected\n";

        // Set SNI hostname for TLS
        if (!SSL_set_tlsext_host_name(
                ws.next_layer().native_handle(),
                host.c_str()))
        {
            throw std::runtime_error("Failed to set SNI hostname");
        }

        // Verify Coinbase's TLS certificate
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

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}