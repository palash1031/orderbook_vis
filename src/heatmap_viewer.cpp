#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

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
    std::uint16_t port = 8080;
};

struct ViewerAssets
{
    std::string index;
    std::string styles;
    std::string script;
    std::string heatmap;
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
        << " [--heatmap heatmap.json] [--port 8080] [--web-root web]\n";
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

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h")
        {
            print_usage(argv[0]);
            return std::nullopt;
        }

        if (index + 1 >= argc)
        {
            throw std::invalid_argument("Missing value for " + argument);
        }

        const std::string value = argv[++index];

        if (argument == "--heatmap")
        {
            options.heatmap_path = value;
        }
        else if (argument == "--port")
        {
            options.port = parse_port(value);
        }
        else if (argument == "--web-root")
        {
            options.web_root = value;
        }
        else
        {
            throw std::invalid_argument("Unknown viewer option: " + argument);
        }
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

ViewerAssets load_assets(const ViewerOptions& options)
{
    return {
        read_file(options.web_root / "index.html"),
        read_file(options.web_root / "styles.css"),
        read_file(options.web_root / "app.js"),
        read_file(options.heatmap_path)
    };
}

std::optional<Route> route_request(
    beast::string_view target,
    const ViewerAssets& assets)
{
    if (target == "/" || target == "/index.html")
    {
        return Route{&assets.index, "text/html; charset=utf-8"};
    }

    if (target == "/styles.css")
    {
        return Route{&assets.styles, "text/css; charset=utf-8"};
    }

    if (target == "/app.js")
    {
        return Route{&assets.script, "text/javascript; charset=utf-8"};
    }

    if (target == "/api/heatmap")
    {
        return Route{&assets.heatmap, "application/json"};
    }

    return std::nullopt;
}

template <typename Body, typename Allocator>
http::response<http::string_body> make_response(
    const http::request<Body, http::basic_fields<Allocator>>& request,
    const ViewerAssets& assets)
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

    if (request.method() != http::verb::get && request.method() != http::verb::head)
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
        response.body() = R"({"status":"ok"})";
    }
    else
    {
        const std::optional<Route> route = route_request(
            request.target(),
            assets
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

void serve(const ViewerOptions& options, const ViewerAssets& assets)
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

        try
        {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            auto response = make_response(request, assets);
            http::write(socket, response);
            beast::error_code ignored;
            socket.shutdown(tcp::socket::shutdown_send, ignored);
        }
        catch (const std::exception& error)
        {
            std::cerr << "Request failed: " << error.what() << '\n';
        }
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

        const ViewerAssets assets = load_assets(*options);
        serve(*options, assets);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
