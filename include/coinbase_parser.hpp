#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct BookUpdate
{
    std::string side;
    double price;
    double quantity;
};

enum class BookEventType
{
    Snapshot,
    Update
};

struct ParsedBookMessage
{
    std::uint64_t sequence_num;
    BookEventType type;
    std::vector<BookUpdate> updates;
};

class CoinbaseParser
{
public:
    static std::optional<ParsedBookMessage>
    parse(const std::string& raw_message);
};
