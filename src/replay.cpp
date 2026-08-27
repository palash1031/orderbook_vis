#include <boost/json.hpp>

#include "order_book.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace json = boost::json;

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
            json::value parsed = json::parse(line);

            auto& obj = parsed.as_object();

            std::string channel =
                obj.at("channel").as_string().c_str();

            if (channel != "l2_data")
            {
                continue;
            }

            auto& events = obj.at("events").as_array();

            for (const auto& event_value : events)
            {
                const auto& event = event_value.as_object();
                const auto& updates = event.at("updates").as_array();

                for (const auto& update_value : updates)
                {
                    const auto& update = update_value.as_object();

                    std::string side =
                        update.at("side").as_string().c_str();

                    double price =
                        std::stod(
                            update.at("price_level")
                                .as_string()
                                .c_str()
                        );

                    double quantity =
                        std::stod(
                            update.at("new_quantity")
                                .as_string()
                                .c_str()
                        );

                    book.apply_update(
                        side,
                        price,
                        quantity
                    );
                }
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
