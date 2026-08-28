#include "book_reconstructor.hpp"
#include "coinbase_parser.hpp"

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

    BookReconstructor reconstructor;
    SequenceTracker sequence_tracker;

    std::string line;
    int message_count = 0;
    int l2_messages = 0;
    int sequence_gaps = 0;
    int duplicate_messages = 0;
    int stale_messages = 0;

    while (std::getline(input_file, line))
    {
        ++message_count;

        try
        {
            const ParsedCoinbaseMessage parsed =
                CoinbaseParser::parse_message(line);

            const SequenceResult sequence =
                sequence_tracker.observe(parsed.sequence_num);

            switch (sequence.status)
            {
                case SequenceStatus::Gap:
                    ++sequence_gaps;
                    reconstructor.mark_desynchronized();
                    std::cerr
                        << "message "
                        << message_count
                        << " sequence gap: expected "
                        << sequence.expected
                        << ", received "
                        << sequence.received
                        << "; updates ignored until next snapshot\n";
                    break;

                case SequenceStatus::Duplicate:
                    ++duplicate_messages;
                    std::cerr
                        << "message "
                        << message_count
                        << " duplicate sequence: "
                        << sequence.received
                        << "; message ignored\n";
                    break;

                case SequenceStatus::Stale:
                    ++stale_messages;
                    std::cerr
                        << "message "
                        << message_count
                        << " stale sequence: expected "
                        << sequence.expected
                        << ", received "
                        << sequence.received
                        << "; message ignored\n";
                    break;

                case SequenceStatus::First:
                case SequenceStatus::InOrder:
                    break;
            }

            if (!parsed.book_message)
            {
                continue;
            }

            ++l2_messages;

            const ReconstructionResult result = reconstructor.process(
                *parsed.book_message,
                sequence
            );

            if (
                result.applied
                && parsed.book_message->type == BookEventType::Snapshot
                && sequence.status == SequenceStatus::Gap
            )
            {
                sequence_tracker.reset();
                sequence_tracker.observe(parsed.sequence_num);
            }

            if (
                !result.applied
                && parsed.book_message->type == BookEventType::Update
                && (
                    sequence.status == SequenceStatus::First
                    || sequence.status == SequenceStatus::InOrder
                )
            )
            {
                std::cerr
                    << "message "
                    << message_count
                    << " update received without a synchronized snapshot; "
                    << "message ignored\n";
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

    const OrderBook& book = reconstructor.book();

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "Messages processed: " << message_count << '\n';
    std::cout << "L2 messages: " << l2_messages << '\n';
    std::cout << "Sequence gaps: " << sequence_gaps << '\n';
    std::cout << "Duplicate messages: " << duplicate_messages << '\n';
    std::cout << "Stale messages: " << stale_messages << '\n';
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
