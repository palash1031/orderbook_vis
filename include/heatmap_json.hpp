#pragma once

#include "heatmap_history.hpp"

#include <ostream>
#include <string_view>

void write_heatmap_json(
    std::ostream& output,
    std::string_view product_id,
    const HeatmapHistory& heatmap
);
