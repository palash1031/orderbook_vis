#include "kraken_level2_stream.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <openssl/ssl.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;

using tcp = asio::ip::tcp;

namespace
{
constexpr std::string_view kraken_host = "ws.kraken.com";
constexpr std::string_view kraken_service = "443";
constexpr std::string_view kraken_path = "/v2";

std::string json_text(const json::value& value)
{
    const auto& text = value.as_string();
    return std::string(text.c_str(), text.size());
}

std::invalid_argument malformed_message(const std::exception& error)
{
    return std::invalid_argument(
        std::string("Malformed Kraken WebSocket message: ") + error.what()
    );
}

json::object parse_object(std::string_view raw_message)
{
    try
    {
        return json::parse(raw_message).as_object();
    }
    catch (const std::exception& error)
    {
        throw malformed_message(error);
    }
}

std::string message_channel(const json::object& message)
{
    if (const auto* channel = message.if_contains("channel"))
    {
        if (!channel->is_string())
        {
            throw std::invalid_argument(
                "Malformed Kraken WebSocket channel"
            );
        }

        return json_text(*channel);
    }

    return {};
}

std::string acknowledgement_channel(const json::object& message)
{
    const auto* result_value = message.if_contains("result");

    if (!result_value || !result_value->is_object())
    {
        return {};
    }

    const auto* channel = result_value->as_object().if_contains("channel");

    if (!channel)
    {
        return {};
    }

    if (!channel->is_string())
    {
        throw std::invalid_argument(
            "Malformed Kraken subscription acknowledgement"
        );
    }

    return json_text(*channel);
}

bool is_subscription_acknowledgement(const json::object& message)
{
    const auto* method = message.if_contains("method");
    return method
        && method->is_string()
        && method->as_string() == "subscribe";
}

std::string ascii_lowercase(std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());

    for (const char character : text)
    {
        if (character >= 'A' && character <= 'Z')
        {
            normalized.push_back(
                static_cast<char>(character - 'A' + 'a')
            );
        }
        else
        {
            normalized.push_back(character);
        }
    }

    return normalized;
}

bool is_permanent_market_rejection(std::string_view error)
{
    const std::string normalized = ascii_lowercase(error);
    return normalized.find("pair not supported") != std::string::npos
        || normalized.find("symbol not supported") != std::string::npos
        || normalized.find("unknown symbol") != std::string::npos;
}

void validate_subscription_acknowledgement(
    const json::object& message,
    std::string_view expected_channel)
{
    const std::string channel = acknowledgement_channel(message);

    if (!channel.empty() && channel != expected_channel)
    {
        return;
    }

    const auto* success = message.if_contains("success");

    if (!success || !success->is_bool())
    {
        throw std::invalid_argument(
            "Malformed Kraken subscription acknowledgement"
        );
    }

    if (success->as_bool())
    {
        return;
    }

    std::string detail = "Kraken ";
    detail += expected_channel;
    detail += " subscription rejected";
    std::string rejection_reason;

    if (const auto* error = message.if_contains("error"))
    {
        if (!error->is_string())
        {
            throw std::invalid_argument(
                "Malformed Kraken subscription error"
            );
        }

        detail += ": ";
        rejection_reason = json_text(*error);
        detail += rejection_reason;
    }

    if (expected_channel == "book"
        && is_permanent_market_rejection(rejection_reason))
    {
        throw UnsupportedMarketError(detail);
    }

    throw std::runtime_error(detail);
}

class BeastKrakenWire final : public KrakenWire
{
public:
    BeastKrakenWire()
        : resolver_(context_),
          ssl_context_(ssl::context::tls_client),
          stream_(context_, ssl_context_)
    {
        ssl_context_.set_default_verify_paths();
        const std::string host(kraken_host);
        const auto endpoints = resolver_.resolve(
            kraken_host,
            kraken_service
        );
        beast::get_lowest_layer(stream_).connect(endpoints);

        if (!SSL_set_tlsext_host_name(
                stream_.next_layer().native_handle(),
                host.c_str()
            ))
        {
            throw std::runtime_error(
                "Failed to set Kraken TLS SNI hostname"
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
        stream_.handshake(host, kraken_path);
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

KrakenLevel2Stream::KrakenLevel2Stream(
    std::string_view product_id,
    std::size_t depth)
    : product_(Product::parse(product_id)),
      depth_(depth)
{
    if (!is_supported_kraken_book_depth(depth_))
    {
        throw std::invalid_argument("Unsupported Kraken book depth");
    }

    wire_ = std::make_unique<BeastKrakenWire>();
    initialize();
}

KrakenLevel2Stream::KrakenLevel2Stream(
    std::string_view product_id,
    std::unique_ptr<KrakenWire> wire,
    std::size_t depth)
    : KrakenLevel2Stream(
          Product::parse(product_id),
          depth,
          std::move(wire)
      )
{
}

KrakenLevel2Stream::KrakenLevel2Stream(
    Product product,
    std::size_t depth,
    std::unique_ptr<KrakenWire> wire)
    : product_(std::move(product)),
      depth_(depth),
      wire_(std::move(wire))
{
    if (!is_supported_kraken_book_depth(depth_))
    {
        throw std::invalid_argument("Unsupported Kraken book depth");
    }

    if (!wire_)
    {
        throw std::invalid_argument("Kraken stream requires a wire");
    }

    initialize();
}

KrakenLevel2Stream::~KrakenLevel2Stream() = default;
KrakenLevel2Stream::KrakenLevel2Stream(KrakenLevel2Stream&&) noexcept = default;
KrakenLevel2Stream& KrakenLevel2Stream::operator=(
    KrakenLevel2Stream&&) noexcept = default;

void KrakenLevel2Stream::initialize()
{
    wire_->write(make_kraken_instrument_subscription());

    while (true)
    {
        const std::string raw_message = wire_->read();
        const json::object message = parse_object(raw_message);

        if (is_subscription_acknowledgement(message))
        {
            validate_subscription_acknowledgement(message, "instrument");
            continue;
        }

        const std::string channel = message_channel(message);

        if (channel == "status" || channel == "heartbeat")
        {
            continue;
        }

        if (channel != "instrument")
        {
            throw std::invalid_argument(
                "Expected Kraken instrument discovery metadata"
            );
        }

        const KrakenInstrumentCatalog catalog =
            KrakenInstrumentCatalog::parse(raw_message);
        const std::optional<std::string> native_symbol =
            catalog.native_symbol(product_);

        if (!native_symbol)
        {
            throw UnsupportedMarketError(
                "Kraken does not currently support " + product_.to_string()
            );
        }

        adapter_ = std::make_unique<KrakenBookAdapter>(
            product_,
            *native_symbol,
            depth_
        );
        wire_->write(make_kraken_book_subscription(
            *native_symbol,
            depth_
        ));
        return;
    }
}

TrustedBookEvent KrakenLevel2Stream::read_event()
{
    while (true)
    {
        const std::string raw_message = wire_->read();
        json::object message;

        try
        {
            message = parse_object(raw_message);
        }
        catch (const std::invalid_argument&)
        {
            if (const auto event = adapter_->process(raw_message))
            {
                return *event;
            }

            throw;
        }

        if (is_subscription_acknowledgement(message))
        {
            validate_subscription_acknowledgement(message, "book");
            continue;
        }

        if (message_channel(message) != "book")
        {
            continue;
        }

        if (const auto event = adapter_->process(raw_message))
        {
            return *event;
        }
    }
}

VenueMarketStatus KrakenLevel2Stream::status() const noexcept
{
    return adapter_
        ? adapter_->status()
        : VenueMarketStatus::Connecting;
}
