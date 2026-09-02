#pragma once

#include "heatmap_history.hpp"
#include "market.hpp"

#include <cstdint>
#include <filesystem>
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
    std::uint16_t port = 8080;
    bool live = false;
    bool show_help = false;
};

ViewerOptions parse_viewer_options(
    std::span<const std::string_view> arguments,
    std::filesystem::path default_web_root = "web"
);
