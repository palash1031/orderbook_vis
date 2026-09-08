#include "coinbase_wire.hpp"

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
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;

using tcp = asio::ip::tcp;

namespace
{
constexpr std::string_view coinbase_host =
    "advanced-trade-ws.coinbase.com";
constexpr std::string_view coinbase_service = "443";
constexpr std::string_view coinbase_path = "/";

class BeastCoinbaseWire final : public CoinbaseWire
{
public:
    BeastCoinbaseWire()
        : resolver_(context_),
          ssl_context_(ssl::context::tls_client),
          stream_(context_, ssl_context_)
    {
        ssl_context_.set_default_verify_paths();
        const std::string host(coinbase_host);
        const auto endpoints = resolver_.resolve(
            coinbase_host,
            coinbase_service
        );
        beast::get_lowest_layer(stream_).connect(endpoints);

        if (!SSL_set_tlsext_host_name(
                stream_.next_layer().native_handle(),
                host.c_str()
            ))
        {
            throw std::runtime_error(
                "Failed to set Coinbase TLS SNI hostname"
            );
        }

        stream_.next_layer().set_verify_mode(ssl::verify_peer);
        stream_.next_layer().set_verify_callback(
            ssl::host_name_verification(host)
        );
        stream_.next_layer().handshake(ssl::stream_base::client);
        stream_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client
            )
        );
        stream_.handshake(host, coinbase_path);
    }

    void write(std::string_view message) override
    {
        stream_.write(asio::buffer(message));
    }

    std::string read() override
    {
        stream_.read(buffer_);
        std::string message = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        return message;
    }

private:
    asio::io_context context_;
    tcp::resolver resolver_;
    ssl::context ssl_context_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream_;
    beast::flat_buffer buffer_;
};
}

std::unique_ptr<CoinbaseWire> make_coinbase_wire()
{
    return std::make_unique<BeastCoinbaseWire>();
}
