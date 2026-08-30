#pragma once

#include "heatmap_history.hpp"

#include <ostream>
#include <string>
#include <string_view>

std::string serialize_heatmap_column(const HeatmapColumn& column);

void write_heatmap_json(
    std::ostream& output,
    std::string_view product_id,
    const HeatmapHistory& heatmap
);
