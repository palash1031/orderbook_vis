#include "coinbase_level2_stream.hpp"
#include "live_source.hpp"
#include "live_stream.hpp"
#include "recorder_config.hpp"
#include "replay_config.hpp"
#include "replay_stream.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
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
struct ViewerOptions
{
    std::filesystem::path heatmap_path = "heatmap.json";
    std::filesystem::path web_root = ORDERBOOK_WEB_ROOT;
    std::string product_id = "BTC-USD";
    HeatmapConfig live_heatmap_config;
    std::uint16_t port = 8080;
    bool live = false;
};

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
    std::string metadata;
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
        << " [--heatmap heatmap.json | --live [--product BTC-USD]"
        << " [--price-bin auto|SIZE]] [--port 8080] [--web-root web]\n";
}

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

std::optional<ViewerOptions> parse_options(int argc, char* argv[])
{
    ViewerOptions options;
    bool heatmap_set = false;
    bool product_set = false;
    bool price_bin_set = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h")
        {
            print_usage(argv[0]);
            return std::nullopt;
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

        if (index + 1 >= argc)
        {
            throw std::invalid_argument("Missing value for " + argument);
        }

        const std::string value = argv[++index];

        if (argument == "--heatmap")
        {
            if (heatmap_set)
            {
                throw std::invalid_argument(
                    "--heatmap may be specified once"
                );
            }

            options.heatmap_path = value;
            heatmap_set = true;
        }
        else if (argument == "--port")
        {
            options.port = parse_port(value);
        }
        else if (argument == "--web-root")
        {
            options.web_root = value;
        }
        else if (argument == "--product")
        {
            if (product_set)
            {
                throw std::invalid_argument(
                    "--product may be specified once"
                );
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
        else
        {
            throw std::invalid_argument("Unknown viewer option: " + argument);
        }
    }

    if (options.live && heatmap_set)
    {
        throw std::invalid_argument(
            "--live and --heatmap select different stream sources"
        );
    }

    if (!options.live && (product_set || price_bin_set))
    {
        throw std::invalid_argument(
            "--product and --price-bin require --live"
        );
    }

    return options;
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

    if (options.live)
    {
        state->live = std::make_shared<LiveStreamHub>();
        json::object metadata;
        metadata["type"] = "metadata";
        metadata["stream_mode"] = "live";
        metadata["product_id"] = options.product_id;
        metadata["status"] = "connecting";
        state->metadata = json::serialize(metadata);
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
        response.body() = json::serialize(health);
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

void run_live_source(
    std::string product_id,
    HeatmapConfig heatmap_config,
    std::shared_ptr<LiveStreamHub> hub)
{
    LiveSourceRunner runner(
        product_id,
        std::move(heatmap_config),
        std::move(hub),
        [product_id]
        {
            return std::make_unique<CoinbaseLevel2Stream>(product_id);
        }
    );
    runner.run();
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
    const std::shared_ptr<LiveStreamHub>& hub)
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

        buffer.consume(buffer.size());

        if (!write_websocket_message(
                stream,
                error_message("Playback controls are unavailable in live mode")
            ))
        {
            return;
        }
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
                    state->live
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
        const std::optional<ViewerOptions> options = parse_options(argc, argv);

        if (!options)
        {
            return 0;
        }

        const std::shared_ptr<const ServerState> state = load_state(*options);

        if (options->live)
        {
            std::thread(
                run_live_source,
                options->product_id,
                options->live_heatmap_config,
                state->live
            ).detach();
        }

        serve(*options, state);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
