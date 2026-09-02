#include "coinbase_level2_stream.hpp"
#include "kraken_level2_stream.hpp"
#include "live_source.hpp"
#include "live_stream.hpp"
#include "replay_config.hpp"
#include "replay_stream.hpp"
#include "viewer_config.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace json = boost::json;

using tcp = asio::ip::tcp;

#ifndef ORDERBOOK_WEB_ROOT
#define ORDERBOOK_WEB_ROOT "web"
#endif

namespace
{
struct ViewerAssets
{
    std::string index;
    std::string styles;
    std::string script;
};

struct ServerState
{
    ViewerAssets assets;
    std::optional<ReplayStreamData> replay;
    std::shared_ptr<LiveStreamHub> live;
    std::shared_ptr<LiveMarketService> live_service;
    std::string metadata;
    Venue venue = Venue::Coinbase;
    bool live_mode = false;
};

struct Route
{
    const std::string* content;
    std::string_view content_type;
};

void print_usage(const char* executable)
{
    std::cout
        << "Usage: "
        << executable
        << " [--heatmap heatmap.json | --live"
        << " [--venue coinbase|kraken] [--product BTC-USD]"
        << " [--price-bin auto|SIZE]] [--port 8080] [--web-root web]\n";
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);

    if (!input.is_open())
    {
        throw std::runtime_error("Failed to open " + path.string());
    }

    std::ostringstream content;
    content << input.rdbuf();

    if (!input.eof() && input.fail())
    {
        throw std::runtime_error("Failed to read " + path.string());
    }

    return content.str();
}

std::shared_ptr<ServerState> load_state(const ViewerOptions& options)
{
    auto state = std::make_shared<ServerState>();
    state->assets = {
        read_file(options.web_root / "index.html"),
        read_file(options.web_root / "styles.css"),
        read_file(options.web_root / "app.js")
    };
    state->live_mode = options.live;
    state->venue = options.venue;

    if (options.live)
    {
        state->live = std::make_shared<LiveStreamHub>();
        state->live_service = std::make_shared<LiveMarketService>(
            options.product_id,
            options.live_heatmap_config,
            state->live,
            [venue = options.venue](std::string_view product_id)
                -> std::unique_ptr<LiveMessageSource>
            {
                if (venue == Venue::Kraken)
                {
                    return std::make_unique<KrakenLevel2Stream>(product_id);
                }

                return std::make_unique<CoinbaseLevel2Stream>(product_id);
            },
            ReconnectBackoffConfig{},
            LiveSourceSleeper{},
            venue_name(options.venue)
        );
    }
    else
    {
        state->replay = parse_replay_stream(read_file(options.heatmap_path));
        state->metadata = state->replay->hello_message_json;
    }

    return state;
}

std::optional<Route> route_request(
    beast::string_view target,
    const ServerState& state)
{
    if (target == "/" || target == "/index.html")
    {
        return Route{&state.assets.index, "text/html; charset=utf-8"};
    }

    if (target == "/styles.css")
    {
        return Route{&state.assets.styles, "text/css; charset=utf-8"};
    }

    if (target == "/app.js")
    {
        return Route{&state.assets.script, "text/javascript; charset=utf-8"};
    }

    if (target == "/api/metadata")
    {
        return Route{&state.metadata, "application/json"};
    }

    return std::nullopt;
}

template <typename Body, typename Allocator>
http::response<http::string_body> make_response(
    const http::request<Body, http::basic_fields<Allocator>>& request,
    const ServerState& state)
{
    http::response<http::string_body> response;
    response.version(request.version());
    response.keep_alive(false);
    response.set(http::field::server, "orderbook-heatmap-viewer");
    response.set(http::field::x_content_type_options, "nosniff");
    response.set(http::field::cache_control, "no-store");
    response.set(
        http::field::content_security_policy,
        "default-src 'self'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; img-src 'self' data:"
    );

    if (
        request.method() != http::verb::get
        && request.method() != http::verb::head
    )
    {
        response.result(http::status::method_not_allowed);
        response.set(http::field::allow, "GET, HEAD");
        response.set(http::field::content_type, "text/plain; charset=utf-8");
        response.body() = "Method not allowed\n";
        response.prepare_payload();
        return response;
    }

    if (request.target() == "/health")
    {
        response.result(http::status::ok);
        response.set(http::field::content_type, "application/json");
        json::object health;
        health["status"] = "ok";
        health["stream"] = "/ws/heatmap";
        health["mode"] = state.live_mode ? "live" : "replay";
        if (state.live_mode)
        {
            health["product_id"] = state.live_service->product_id();
            health["venue"] = venue_name(state.venue);
        }
        response.body() = json::serialize(health);
    }
    else if (request.target() == "/api/metadata" && state.live_mode)
    {
        response.result(http::status::ok);
        response.set(http::field::content_type, "application/json");
        json::object metadata;
        metadata["type"] = "metadata";
        metadata["stream_mode"] = "live";
        metadata["product_id"] = state.live_service->product_id();
        metadata["venue"] = venue_name(state.venue);
        response.body() = json::serialize(metadata);
    }
    else
    {
        const std::optional<Route> route = route_request(
            request.target(),
            state
        );

        if (!route)
        {
            response.result(http::status::not_found);
            response.set(
                http::field::content_type,
                "text/plain; charset=utf-8"
            );
            response.body() = "Not found\n";
        }
        else
        {
            response.result(http::status::ok);
            response.set(http::field::content_type, route->content_type);
            response.body() = *route->content;
        }
    }

    const std::size_t content_length = response.body().size();

    if (request.method() == http::verb::head)
    {
        response.body().clear();
        response.content_length(content_length);
    }
    else
    {
        response.prepare_payload();
    }

    return response;
}

std::string error_message(std::string_view message)
{
    json::object payload;
    payload["type"] = "error";
    payload["message"] = message;
    return json::serialize(payload);
}

bool write_websocket_message(
    websocket::stream<tcp::socket>& stream,
    std::string_view message)
{
    beast::error_code error;
    stream.text(true);
    stream.write(asio::buffer(message), error);
    return !error;
}

void run_live_source(std::shared_ptr<LiveMarketService> service)
{
    service->run();
}

void run_replay_websocket(
    tcp::socket socket,
    http::request<http::string_body> request,
    const ReplayStreamData& replay)
{
    websocket::stream<tcp::socket> stream{std::move(socket)};
    stream.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response)
        {
            response.set(http::field::server, "orderbook-replay-stream");
        }
    ));
    stream.read_message_max(4 * 1024);
    stream.accept(request);

    ReplayPlayback playback(replay);

    if (
        !write_websocket_message(stream, replay.hello_message_json)
        || !write_websocket_message(stream, playback.state_message())
    )
    {
        return;
    }

    beast::flat_buffer buffer;
    auto next_column_at = std::chrono::steady_clock::now();

    while (true)
    {
        beast::error_code error;
        const std::size_t available = stream.next_layer().available(error);

        if (error)
        {
            return;
        }

        if (available > 0)
        {
            stream.read(buffer, error);

            if (error == websocket::error::closed)
            {
                return;
            }

            if (error)
            {
                return;
            }

            if (!stream.got_text())
            {
                if (!write_websocket_message(
                        stream,
                        error_message("Replay controls must be JSON text")
                    ))
                {
                    return;
                }
            }
            else
            {
                const std::string command = beast::buffers_to_string(
                    buffer.data()
                );

                try
                {
                    const ReplayControlResult result =
                        playback.apply_control(command);

                    if (
                        result == ReplayControlResult::Restarted
                        && !write_websocket_message(
                            stream,
                            R"({"type":"reset"})"
                        )
                    )
                    {
                        return;
                    }

                    if (!write_websocket_message(stream, playback.state_message()))
                    {
                        return;
                    }

                    next_column_at = std::chrono::steady_clock::now();
                }
                catch (const std::exception& control_error)
                {
                    if (!write_websocket_message(
                            stream,
                            error_message(control_error.what())
                        ))
                    {
                        return;
                    }
                }
            }

            buffer.consume(buffer.size());
        }

        const auto now = std::chrono::steady_clock::now();

        if (
            playback.status() == ReplayStatus::Playing
            && now >= next_column_at
        )
        {
            const std::optional<std::string> column =
                playback.take_next_column_message();

            if (column && !write_websocket_message(stream, *column))
            {
                return;
            }

            if (playback.status() == ReplayStatus::Complete)
            {
                if (!write_websocket_message(stream, playback.state_message()))
                {
                    return;
                }
            }
            else
            {
                next_column_at = std::chrono::steady_clock::now()
                    + playback.interval_to_next();
            }
        }

        const auto maximum_sleep = std::chrono::milliseconds{3};
        auto sleep_duration = maximum_sleep;

        if (playback.status() == ReplayStatus::Playing)
        {
            sleep_duration = std::min(
                maximum_sleep,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::max(
                        next_column_at - std::chrono::steady_clock::now(),
                        std::chrono::steady_clock::duration::zero()
                    )
                )
            );
        }

        if (sleep_duration.count() > 0)
        {
            std::this_thread::sleep_for(sleep_duration);
        }
    }
}

void run_live_websocket(
    tcp::socket socket,
    http::request<http::string_body> request,
    const std::shared_ptr<LiveStreamHub>& hub,
    const std::shared_ptr<LiveMarketService>& service)
{
    websocket::stream<tcp::socket> stream{std::move(socket)};
    stream.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& response)
        {
            response.set(http::field::server, "orderbook-live-stream");
        }
    ));
    stream.read_message_max(4 * 1024);
    stream.accept(request);

    const std::shared_ptr<LiveStreamSubscriber> subscriber = hub->subscribe();
    beast::flat_buffer buffer;

    while (true)
    {
        if (const auto message = subscriber->wait_for_message(
                std::chrono::milliseconds{50}
            ))
        {
            if (!write_websocket_message(stream, *message))
            {
                return;
            }
        }

        beast::error_code error;
        const std::size_t available = stream.next_layer().available(error);

        if (error)
        {
            return;
        }

        if (available == 0)
        {
            continue;
        }

        stream.read(buffer, error);

        if (error == websocket::error::closed)
        {
            return;
        }

        if (error)
        {
            return;
        }

        if (!stream.got_text())
        {
            if (!write_websocket_message(
                    stream,
                    error_message("Live controls must be JSON text")
                ))
            {
                return;
            }
        }
        else
        {
            const std::string command = beast::buffers_to_string(
                buffer.data()
            );

            try
            {
                service->apply_control(command);
            }
            catch (const std::exception& control_error)
            {
                if (!write_websocket_message(
                        stream,
                        error_message(control_error.what())
                    ))
                {
                    return;
                }
            }
        }

        buffer.consume(buffer.size());
    }
}

void handle_connection(
    tcp::socket socket,
    std::shared_ptr<const ServerState> state)
{
    try
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);

        if (
            websocket::is_upgrade(request)
            && (
                request.target() == "/ws/heatmap"
                || request.target() == "/ws/replay"
            )
        )
        {
            if (state->live_mode)
            {
                run_live_websocket(
                    std::move(socket),
                    std::move(request),
                    state->live,
                    state->live_service
                );
            }
            else
            {
                run_replay_websocket(
                    std::move(socket),
                    std::move(request),
                    *state->replay
                );
            }

            return;
        }

        auto response = make_response(request, *state);
        http::write(socket, response);
        beast::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_send, ignored);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Request failed: " << error.what() << '\n';
    }
}

void serve(
    const ViewerOptions& options,
    std::shared_ptr<const ServerState> state)
{
    asio::io_context context{1};
    const tcp::endpoint endpoint{
        asio::ip::address_v4::loopback(),
        options.port
    };
    tcp::acceptor acceptor{context, endpoint};

    std::cout
        << "Order book heatmap: http://127.0.0.1:"
        << options.port
        << '\n';

    while (true)
    {
        tcp::socket socket{context};
        acceptor.accept(socket);
        std::thread(
            handle_connection,
            std::move(socket),
            state
        ).detach();
    }
}
}

int main(int argc, char* argv[])
{
    try
    {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 1));

        for (int index = 1; index < argc; ++index)
        {
            arguments.emplace_back(argv[index]);
        }

        const ViewerOptions options = parse_viewer_options(
            arguments,
            ORDERBOOK_WEB_ROOT
        );

        if (options.show_help)
        {
            print_usage(argv[0]);
            return 0;
        }

        const std::shared_ptr<const ServerState> state = load_state(options);

        if (options.live)
        {
            std::thread(
                run_live_source,
                state->live_service
            ).detach();
        }

        serve(options, state);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
