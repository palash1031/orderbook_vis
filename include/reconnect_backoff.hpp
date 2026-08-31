#pragma once

#include <chrono>
#include <cstdint>
#include <random>

struct ReconnectBackoffConfig
{
    std::chrono::milliseconds initial_delay{1'000};
    std::chrono::milliseconds maximum_delay{30'000};
    double jitter_fraction = 0.2;
};

class ReconnectBackoff
{
public:
    explicit ReconnectBackoff(ReconnectBackoffConfig config = {});
    ReconnectBackoff(ReconnectBackoffConfig config, std::uint64_t seed);

    std::chrono::milliseconds next_delay();
    void reset() noexcept;

private:
    ReconnectBackoffConfig config_;
    std::chrono::milliseconds nominal_delay_;
    std::mt19937_64 random_;
};
