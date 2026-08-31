#include "reconnect_backoff.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace
{
void validate_config(const ReconnectBackoffConfig& config)
{
    if (
        config.initial_delay.count() <= 0
        || config.maximum_delay < config.initial_delay
        || !std::isfinite(config.jitter_fraction)
        || config.jitter_fraction < 0.0
        || config.jitter_fraction >= 1.0
    )
    {
        throw std::invalid_argument("Invalid reconnect backoff configuration");
    }
}
}

ReconnectBackoff::ReconnectBackoff(ReconnectBackoffConfig config)
    : ReconnectBackoff(config, std::random_device{}())
{
}

ReconnectBackoff::ReconnectBackoff(
    ReconnectBackoffConfig config,
    std::uint64_t seed)
    : config_(config),
      nominal_delay_(config.initial_delay),
      random_(seed)
{
    validate_config(config_);
}

std::chrono::milliseconds ReconnectBackoff::next_delay()
{
    std::uniform_real_distribution<double> distribution(
        1.0 - config_.jitter_fraction,
        1.0
    );
    const auto randomized_count = static_cast<std::int64_t>(std::llround(
        static_cast<double>(nominal_delay_.count()) * distribution(random_)
    ));
    const std::chrono::milliseconds delay{
        std::max<std::int64_t>(1, randomized_count)
    };

    if (nominal_delay_ < config_.maximum_delay)
    {
        const auto remaining = config_.maximum_delay - nominal_delay_;
        nominal_delay_ += std::min(nominal_delay_, remaining);
    }

    return delay;
}

void ReconnectBackoff::reset() noexcept
{
    nominal_delay_ = config_.initial_delay;
}
