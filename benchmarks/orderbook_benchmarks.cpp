#include "book_reconstructor.hpp"
#include "order_book.hpp"
#include "sequence_tracker.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t baseline_level_count = 1'000;
constexpr std::size_t mixed_batch_size = 1'000;

void populate_baseline_book(OrderBook& book)
{
    book.clear();

    const std::string bid_side = "bid";
    const std::string ask_side = "offer";

    for (std::size_t index = 0; index < baseline_level_count; ++index)
    {
        book.apply_update(
            bid_side,
            10'000.0 + static_cast<double>(index),
            1.0
        );
        book.apply_update(
            ask_side,
            20'000.0 + static_cast<double>(index),
            1.0
        );
    }
}

std::vector<BookUpdate> make_mixed_updates()
{
    std::vector<BookUpdate> updates;
    updates.reserve(mixed_batch_size);

    for (std::size_t index = 0; index < mixed_batch_size / 4; ++index)
    {
        updates.push_back({
            "bid",
            15'000.0 + static_cast<double>(index),
            2.0
        });
        updates.push_back({
            "offer",
            40'000.0 + static_cast<double>(index),
            2.0
        });
        updates.push_back({
            "bid",
            10'000.0 + static_cast<double>(index),
            3.0
        });
        updates.push_back({
            "offer",
            20'000.0 + static_cast<double>(index),
            0.0
        });
    }

    return updates;
}

struct ReconstructionWorkload
{
    ParsedBookMessage snapshot;
    std::vector<ParsedBookMessage> updates;
    std::size_t update_count;
};

ReconstructionWorkload make_reconstruction_workload()
{
    ReconstructionWorkload workload{
        {0, BookEventType::Snapshot, {}},
        {},
        0
    };

    workload.snapshot.updates.reserve(2 * baseline_level_count);

    for (std::size_t index = 0; index < baseline_level_count; ++index)
    {
        workload.snapshot.updates.push_back({
            "bid",
            10'000.0 + static_cast<double>(index),
            1.0
        });
        workload.snapshot.updates.push_back({
            "offer",
            20'000.0 + static_cast<double>(index),
            1.0
        });
    }

    constexpr std::size_t message_count = 100;
    constexpr std::size_t updates_per_message = 10;
    workload.updates.reserve(message_count);

    for (std::size_t message_index = 0;
         message_index < message_count;
         ++message_index)
    {
        ParsedBookMessage message{
            static_cast<std::uint64_t>(message_index + 1),
            BookEventType::Update,
            {}
        };
        message.updates.reserve(updates_per_message);

        for (std::size_t update_index = 0;
             update_index < updates_per_message;
             ++update_index)
        {
            const std::size_t index =
                message_index * updates_per_message + update_index;

            switch (index % 4)
            {
                case 0:
                    message.updates.push_back({
                        "bid",
                        10'000.0 + static_cast<double>(index),
                        2.0
                    });
                    break;

                case 1:
                    message.updates.push_back({
                        "offer",
                        20'000.0 + static_cast<double>(index),
                        2.0
                    });
                    break;

                case 2:
                    message.updates.push_back({
                        "bid",
                        15'000.0 + static_cast<double>(index),
                        1.0
                    });
                    break;

                case 3:
                    message.updates.push_back({
                        "offer",
                        20'000.0 + static_cast<double>(index),
                        0.0
                    });
                    break;
            }
        }

        workload.update_count += message.updates.size();
        workload.updates.push_back(std::move(message));
    }

    return workload;
}

void insert_new_bid(benchmark::State& state)
{
    OrderBook book;
    populate_baseline_book(book);

    const std::string bid_side = "bid";
    constexpr double inserted_price = 10'500.5;

    for (auto _ : state)
    {
        book.apply_update(bid_side, inserted_price, 2.0);
        benchmark::ClobberMemory();

        state.PauseTiming();
        benchmark::DoNotOptimize(book.bid_levels());
        book.apply_update(bid_side, inserted_price, 0.0);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations());
}

void update_existing_bid(benchmark::State& state)
{
    OrderBook book;
    populate_baseline_book(book);

    const std::string bid_side = "bid";
    constexpr double existing_price = 10'500.0;
    double quantity = 1.0;

    for (auto _ : state)
    {
        quantity += 0.000001;
        book.apply_update(bid_side, existing_price, quantity);
        benchmark::ClobberMemory();
    }

    benchmark::DoNotOptimize(book.bid_levels());
    state.SetItemsProcessed(state.iterations());
}

void erase_bid(benchmark::State& state)
{
    OrderBook book;
    populate_baseline_book(book);

    const std::string bid_side = "bid";
    constexpr double erased_price = 10'500.0;

    for (auto _ : state)
    {
        book.apply_update(bid_side, erased_price, 0.0);
        benchmark::ClobberMemory();

        state.PauseTiming();
        benchmark::DoNotOptimize(book.bid_levels());
        book.apply_update(bid_side, erased_price, 1.0);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations());
}

void mixed_book_updates(benchmark::State& state)
{
    const std::vector<BookUpdate> updates = make_mixed_updates();
    OrderBook book;
    populate_baseline_book(book);

    for (auto _ : state)
    {
        for (const auto& update : updates)
        {
            book.apply_update(
                update.side,
                update.price,
                update.quantity
            );
        }
        benchmark::ClobberMemory();

        state.PauseTiming();
        benchmark::DoNotOptimize(book.bid_levels());
        benchmark::DoNotOptimize(book.ask_levels());
        populate_baseline_book(book);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(updates.size())
    );
}

void reconstruct_batch(benchmark::State& state)
{
    const ReconstructionWorkload workload =
        make_reconstruction_workload();
    const std::size_t items_per_iteration =
        workload.snapshot.updates.size() + workload.update_count;

    for (auto _ : state)
    {
        BookReconstructor reconstructor;
        SequenceTracker tracker;

        const SequenceResult snapshot_sequence =
            tracker.observe(workload.snapshot.sequence_num);
        reconstructor.process(workload.snapshot, snapshot_sequence);

        for (const auto& message : workload.updates)
        {
            const SequenceResult sequence =
                tracker.observe(message.sequence_num);
            reconstructor.process(message, sequence);
        }

        benchmark::DoNotOptimize(reconstructor.book().best_bid());
        benchmark::DoNotOptimize(reconstructor.book().best_ask());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
        * static_cast<std::int64_t>(items_per_iteration)
    );
}
}

BENCHMARK(insert_new_bid)->Name("OrderBook/InsertNewBid");
BENCHMARK(update_existing_bid)->Name("OrderBook/UpdateExistingBid");
BENCHMARK(erase_bid)->Name("OrderBook/EraseBid");
BENCHMARK(mixed_book_updates)->Name("OrderBook/MixedBookUpdates");
BENCHMARK(reconstruct_batch)->Name("Reconstruction/ReconstructBatch");
