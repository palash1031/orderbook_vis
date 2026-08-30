#include "book_reconstructor.hpp"
#include "coinbase_parser.hpp"
#include "heatmap_history.hpp"
#include "heatmap_json.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
struct ReplayStats
{
    std::uint64_t lines_read = 0;
    std::uint64_t valid_coinbase_messages = 0;
    std::uint64_t malformed_messages = 0;
    std::uint64_t non_l2_messages = 0;
    std::uint64_t l2_messages = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t update_messages = 0;
    std::uint64_t book_updates_received = 0;
    std::uint64_t book_updates_applied = 0;
    std::uint64_t ignored_l2_messages = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t duplicate_messages = 0;
    std::uint64_t stale_messages = 0;
    std::uint64_t heatmap_samples = 0;
};

struct ReplayOptions
{
    std::string input_path = "btc_usd.jsonl";
    std::optional<std::string> heatmap_output_path;
};

void print_usage(const char* executable)
{
    std::cout
        << "Usage: "
        << executable
        << " [capture.jsonl] [--heatmap-output heatmap.json]\n";
}

std::optional<ReplayOptions> parse_options(int argc, char* argv[])
{
    ReplayOptions options;
    bool input_path_set = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h")
        {
            print_usage(argv[0]);
            return std::nullopt;
        }

        if (argument == "--heatmap-output")
        {
            if (index + 1 >= argc || options.heatmap_output_path)
            {
                throw std::invalid_argument(
                    "--heatmap-output requires exactly one path"
                );
            }

            options.heatmap_output_path = argv[++index];
            continue;
        }

        if (argument.starts_with('-') || input_path_set)
        {
            throw std::invalid_argument(
                "Unexpected replay argument: " + argument
            );
        }

        options.input_path = argument;
        input_path_set = true;
    }

    return options;
}

double per_second(std::uint64_t count, double elapsed_seconds)
{
    if (elapsed_seconds <= 0.0)
    {
        return 0.0;
    }

    return static_cast<double>(count) / elapsed_seconds;
}
}

int main(int argc, char* argv[])
{
    std::optional<ReplayOptions> parsed_options;

    try
    {
        parsed_options = parse_options(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }

    if (!parsed_options)
    {
        return 0;
    }

    const ReplayOptions& options = *parsed_options;
    std::ifstream input_file(options.input_path);

    if (!input_file.is_open())
    {
        std::cerr << "Failed to open " << options.input_path << '\n';
        return 1;
    }

    BookReconstructor reconstructor;
    SequenceTracker sequence_tracker;
    HeatmapHistory heatmap;

    std::string line;
    std::string product_id;
    ReplayStats stats;

    const auto replay_start = std::chrono::steady_clock::now();

    while (std::getline(input_file, line))
    {
        ++stats.lines_read;

        try
        {
            const ParsedCoinbaseMessage parsed =
                CoinbaseParser::parse_message(line);
            ++stats.valid_coinbase_messages;

            if (parsed.book_message)
            {
                if (product_id.empty())
                {
                    product_id = parsed.book_message->product_id;
                }
                else if (product_id != parsed.book_message->product_id)
                {
                    throw std::invalid_argument(
                        "Capture contains multiple product IDs"
                    );
                }
            }

            const SequenceResult sequence =
                sequence_tracker.observe(parsed.sequence_num);
            const bool is_snapshot =
                parsed.book_message
                && parsed.book_message->type == BookEventType::Snapshot;

            switch (sequence.status)
            {
                case SequenceStatus::Gap:
                    ++stats.sequence_gaps;
                    reconstructor.mark_desynchronized();
                    heatmap.mark_discontinuity();
                    std::cerr
                        << "message "
                        << stats.lines_read
                        << " sequence gap: expected "
                        << sequence.expected
                        << ", received "
                        << sequence.received;

                    if (is_snapshot)
                    {
                        std::cerr
                            << "; snapshot will be used as recovery point\n";
                    }
                    else
                    {
                        std::cerr
                            << "; updates ignored until next snapshot\n";
                    }
                    break;

                case SequenceStatus::Duplicate:
                    ++stats.duplicate_messages;
                    std::cerr
                        << "message "
                        << stats.lines_read
                        << " duplicate sequence: "
                        << sequence.received
                        << "; message ignored\n";
                    break;

                case SequenceStatus::Stale:
                    ++stats.stale_messages;
                    std::cerr
                        << "message "
                        << stats.lines_read
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
                ++stats.non_l2_messages;
                continue;
            }

            ++stats.l2_messages;

            if (parsed.book_message->type == BookEventType::Snapshot)
            {
                ++stats.snapshots;
            }
            else
            {
                ++stats.update_messages;
            }

            stats.book_updates_received +=
                parsed.book_message->updates.size();

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

            if (result.applied)
            {
                stats.book_updates_applied +=
                    parsed.book_message->updates.size();

                if (
                    heatmap.sample(
                        parsed.book_message->timestamp,
                        reconstructor.book()
                    )
                )
                {
                    ++stats.heatmap_samples;
                }
            }
            else
            {
                ++stats.ignored_l2_messages;
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
                    << stats.lines_read
                    << " update received without a synchronized snapshot; "
                    << "message ignored\n";
            }
        }
        catch (const std::exception& e)
        {
            ++stats.malformed_messages;
            std::cerr
                << "message "
                << stats.lines_read
                << " failed to parse/process: "
                << e.what()
                << '\n';
        }
    }

    const auto replay_end = std::chrono::steady_clock::now();
    const double elapsed_seconds =
        std::chrono::duration<double>(replay_end - replay_start).count();

    const OrderBook& book = reconstructor.book();

    if (options.heatmap_output_path)
    {
        std::ofstream heatmap_output(
            *options.heatmap_output_path,
            std::ios::binary
        );

        if (!heatmap_output.is_open())
        {
            std::cerr
                << "Failed to open "
                << *options.heatmap_output_path
                << '\n';
            return 1;
        }

        try
        {
            write_heatmap_json(heatmap_output, product_id, heatmap);
        }
        catch (const std::exception& error)
        {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    std::cout << "Replay statistics\n";
    std::cout << "-----------------\n";
    std::cout << "Lines read: " << stats.lines_read << '\n';
    std::cout
        << "Valid Coinbase messages: "
        << stats.valid_coinbase_messages
        << '\n';
    std::cout << "Malformed messages: " << stats.malformed_messages << '\n';
    std::cout << "Non-L2 messages: " << stats.non_l2_messages << '\n';
    std::cout << "L2 messages: " << stats.l2_messages << '\n';
    std::cout << "Snapshots: " << stats.snapshots << '\n';
    std::cout << "Update messages: " << stats.update_messages << '\n';
    std::cout
        << "Book updates received: "
        << stats.book_updates_received
        << '\n';
    std::cout
        << "Book updates applied: "
        << stats.book_updates_applied
        << '\n';
    std::cout << "Sequence gaps: " << stats.sequence_gaps << '\n';
    std::cout
        << "Duplicate messages: "
        << stats.duplicate_messages
        << '\n';
    std::cout << "Stale messages: " << stats.stale_messages << '\n';
    std::cout
        << "Ignored L2 messages: "
        << stats.ignored_l2_messages
        << '\n';
    std::cout << "Heatmap samples: " << stats.heatmap_samples << '\n';
    std::cout
        << "Heatmap columns retained: "
        << heatmap.columns().size()
        << '\n';

    if (options.heatmap_output_path)
    {
        std::cout
            << "Heatmap output: "
            << *options.heatmap_output_path
            << '\n';
    }

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Replay elapsed: " << elapsed_seconds << " s\n";

    std::cout << std::setprecision(2);
    std::cout
        << "Valid messages/sec: "
        << per_second(stats.valid_coinbase_messages, elapsed_seconds)
        << '\n';
    std::cout
        << "L2 messages/sec: "
        << per_second(stats.l2_messages, elapsed_seconds)
        << '\n';
    std::cout
        << "Book updates received/sec: "
        << per_second(stats.book_updates_received, elapsed_seconds)
        << '\n';
    std::cout
        << "Book updates applied/sec: "
        << per_second(stats.book_updates_applied, elapsed_seconds)
        << '\n';

    std::cout << "\nBook state\n";
    std::cout << "----------\n";
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
