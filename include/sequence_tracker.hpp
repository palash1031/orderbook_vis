#pragma once

#include <cstdint>
#include <optional>

enum class SequenceStatus
{
    First,
    InOrder,
    Duplicate,
    Stale,
    Gap
};

struct SequenceResult
{
    SequenceStatus status;
    std::uint64_t expected;
    std::uint64_t received;
};

class SequenceTracker
{
public:
    // Duplicate, stale, and gap observations do not advance the last
    // accepted sequence. Call reset() when an authoritative snapshot
    // establishes a new continuity boundary.
    SequenceResult observe(std::uint64_t sequence_num);

    void reset() noexcept;
    bool initialized() const noexcept;

private:
    std::optional<std::uint64_t> last_sequence_;
};
