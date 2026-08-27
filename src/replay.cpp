#include <boost/json.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

namespace json = boost::json;

void apply_update(
    std::map<double, double>& bids,
    std::map<double, double>& asks,
    const std::string& side,
    double price,
    double quantity)
{
    if (side == "bid")
    {
        if (quantity == 0.0)
        {
            bids.erase(price);
        }
        else
        {
            bids[price] = quantity;
        }
    }
    else if (side == "offer")
    {
        if (quantity == 0.0)
        {
            asks.erase(price);
        }
        else
        {
            asks[price] = quantity;
        }
    }
}

int main()
{
    std::ifstream input_file("btc_usd.jsonl");

    if (!input_file.is_open())
    {
        std::cerr << "Failed to open btc_usd.jsonl\n";
        return 1;
    }

    std::map<double, double> bids;
    std::map<double, double> asks;

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

                    apply_update(
                        bids,
                        asks,
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
    std::cout << "Bid levels: " << bids.size() << '\n';
    std::cout << "Ask levels: " << asks.size() << '\n';

    if (!bids.empty() && !asks.empty())
    {
        double best_bid = bids.rbegin()->first;
        double best_ask = asks.begin()->first;
        double spread = best_ask - best_bid;

        std::cout << "Best bid: " << best_bid << '\n';
        std::cout << "Best ask: " << best_ask << '\n';
        std::cout << "Spread: " << spread << '\n';
    }

    return 0;
}