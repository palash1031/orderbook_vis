#include "sequence_tracker.hpp"

#include <limits>

namespace
{
std::uint64_t next_expected(std::uint64_t last_sequence)
{
    if (last_sequence == std::numeric_limits<std::uint64_t>::max())
    {
        return last_sequence;
    }

    return last_sequence + 1;
}
}

SequenceResult SequenceTracker::observe(std::uint64_t sequence_num)
{
    if (!last_sequence_)
    {
        last_sequence_ = sequence_num;
        return {SequenceStatus::First, sequence_num, sequence_num};
    }

    const std::uint64_t last_sequence = *last_sequence_;
    const std::uint64_t expected = next_expected(last_sequence);

    if (sequence_num == last_sequence)
    {
        return {SequenceStatus::Duplicate, expected, sequence_num};
    }

    if (sequence_num < last_sequence)
    {
        return {SequenceStatus::Stale, expected, sequence_num};
    }

    if (sequence_num == expected)
    {
        last_sequence_ = sequence_num;
        return {SequenceStatus::InOrder, expected, sequence_num};
    }

    return {SequenceStatus::Gap, expected, sequence_num};
}

void SequenceTracker::reset() noexcept
{
    last_sequence_.reset();
}

bool SequenceTracker::initialized() const noexcept
{
    return last_sequence_.has_value();
}
