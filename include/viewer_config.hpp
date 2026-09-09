#pragma once

#include "heatmap_history.hpp"
#include "market.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ViewerOptions
{
    std::filesystem::path heatmap_path = "heatmap.json";
    std::filesystem::path web_root = "web";
    std::string product_id = "BTC-USD";
    HeatmapConfig live_heatmap_config;
    Venue venue = Venue::Coinbase;
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 8080;
    bool live = false;
    bool scanner = false;
    bool show_help = false;
    bool public_demo = false;
};

ViewerOptions parse_viewer_options(
    std::span<const std::string_view> arguments,
    std::filesystem::path default_web_root = "web",
    std::optional<std::string_view> environment_port = std::nullopt
);

std::string_view viewer_stream_mode_name(
    const ViewerOptions& options
) noexcept;

std::string_view viewer_websocket_path(
    const ViewerOptions& options
) noexcept;
