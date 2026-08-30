#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ReplayStreamColumn
{
    std::int64_t timestamp_ms;
    std::string payload_json;
};

struct ReplayStreamData
{
    std::string product_id;
    std::string hello_message_json;
    std::vector<ReplayStreamColumn> columns;
};

ReplayStreamData parse_replay_stream(std::string_view document_json);

enum class ReplayStatus
{
    Paused,
    Playing,
    Complete
};

enum class ReplayControlResult
{
    StateChanged,
    Restarted
};

class ReplayPlayback
{
public:
    explicit ReplayPlayback(const ReplayStreamData& replay);

    ReplayControlResult apply_control(std::string_view command_json);
    std::optional<std::string> take_next_column_message();

    std::string state_message() const;
    std::chrono::milliseconds interval_to_next() const noexcept;

    ReplayStatus status() const noexcept;
    std::size_t position() const noexcept;
    unsigned int speed() const noexcept;

private:
    const ReplayStreamData& replay_;
    ReplayStatus status_ = ReplayStatus::Paused;
    std::size_t position_ = 0;
    unsigned int speed_ = 1;
};
