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

struct ParsedCoinbaseMessage
{
    std::uint64_t sequence_num;
    std::optional<ParsedBookMessage> book_message;
};

class CoinbaseParser
{
public:
    static ParsedCoinbaseMessage
    parse_message(const std::string& raw_message);

    static std::optional<ParsedBookMessage>
    parse(const std::string& raw_message);
};
