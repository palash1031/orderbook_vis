#pragma once

#include "heatmap_history.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ReplayOptions
{
    std::string input_path = "btc_usd.jsonl";
    std::optional<std::string> heatmap_output_path;
    HeatmapConfig heatmap_config;
    bool show_help = false;
};

std::optional<double> parse_price_bin_argument(std::string_view value);

ReplayOptions parse_replay_options(
    std::span<const std::string_view> arguments
);
