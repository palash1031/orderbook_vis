#pragma once

#include <span>
#include <string>
#include <string_view>

struct RecorderOptions
{
    std::string product_id = "BTC-USD";
    std::string output_path = "btc_usd.jsonl";
    bool show_help = false;
};

std::string normalize_product_id(std::string_view product_id);
std::string default_capture_path(std::string_view product_id);
std::string make_level2_subscription(std::string_view product_id);

RecorderOptions parse_recorder_options(
    std::span<const std::string_view> arguments
);
