#include "coinbase_level2_stream.hpp"

#include "recorder_config.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <stdexcept>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

struct CoinbaseLevel2Stream::Implementation
{
    asio::io_context context;
    tcp::resolver resolver{context};
    ssl::context ssl_context{ssl::context::tls_client};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream{
        context,
        ssl_context
    };
    beast::flat_buffer buffer;
};

CoinbaseLevel2Stream::CoinbaseLevel2Stream(std::string_view product_id)
    : implementation_(std::make_unique<Implementation>())
{
    constexpr std::string_view host = "advanced-trade-ws.coinbase.com";
    constexpr std::string_view service = "443";
    Implementation& connection = *implementation_;
    connection.ssl_context.set_default_verify_paths();

    const auto endpoints = connection.resolver.resolve(host, service);
    beast::get_lowest_layer(connection.stream).connect(endpoints);

    const std::string host_name(host);

    if (!SSL_set_tlsext_host_name(
            connection.stream.next_layer().native_handle(),
            host_name.c_str()
        ))
    {
        throw std::runtime_error("Failed to set Coinbase TLS SNI hostname");
    }

    connection.stream.next_layer().set_verify_mode(ssl::verify_peer);
    connection.stream.next_layer().set_verify_callback(
        ssl::host_name_verification(host_name)
    );
    connection.stream.next_layer().handshake(ssl::stream_base::client);
    connection.stream.set_option(
        websocket::stream_base::timeout::suggested(
            beast::role_type::client
        )
    );
    connection.stream.handshake(host_name, "/");

    const std::string subscription = make_level2_subscription(product_id);
    connection.stream.write(asio::buffer(subscription));
}

CoinbaseLevel2Stream::~CoinbaseLevel2Stream() = default;
CoinbaseLevel2Stream::CoinbaseLevel2Stream(
    CoinbaseLevel2Stream&&) noexcept = default;
CoinbaseLevel2Stream& CoinbaseLevel2Stream::operator=(
    CoinbaseLevel2Stream&&) noexcept = default;

std::string CoinbaseLevel2Stream::read()
{
    Implementation& connection = *implementation_;
    connection.stream.read(connection.buffer);
    std::string message = beast::buffers_to_string(connection.buffer.data());
    connection.buffer.consume(connection.buffer.size());
    return message;
}
