#pragma once

#include <chrono>
#include <cstdint>

using MarketTimestamp =
    std::chrono::sys_time<std::chrono::nanoseconds>;

enum class BookSide : std::uint8_t
{
    Bid,
    Offer
};
