#include "coinbase_parser.hpp"
#include "order_book.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main()
{
    std::ifstream input_file("btc_usd.jsonl");

    if (!input_file.is_open())
    {
        std::cerr << "Failed to open btc_usd.jsonl\n";
        return 1;
    }

    OrderBook book;

    std::string line;
    int message_count = 0;

    while (std::getline(input_file, line))
    {
        ++message_count;

        try
        {
            const auto parsed = CoinbaseParser::parse(line);

            if (!parsed)
            {
                continue;
            }

            if (parsed->type == BookEventType::Snapshot)
            {
                book.clear();
            }

            for (const auto& update : parsed->updates)
            {
                book.apply_update(
                    update.side,
                    update.price,
                    update.quantity
                );
            }
        }
        catch (const std::exception& e)
        {
            std::cerr
                << "message "
                << message_count
                << " failed to parse/process: "
                << e.what()
                << '\n';
        }
    }

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "Messages processed: " << message_count << '\n';
    std::cout << "Bid levels: " << book.bid_levels() << '\n';
    std::cout << "Ask levels: " << book.ask_levels() << '\n';

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    const auto spread = book.spread();

    if (best_bid && best_ask && spread)
    {
        std::cout << "Best bid: " << *best_bid << '\n';
        std::cout << "Best ask: " << *best_ask << '\n';
        std::cout << "Spread: " << *spread << '\n';
    }

    return 0;
}
