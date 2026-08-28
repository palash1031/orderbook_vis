#pragma once

#include "coinbase_parser.hpp"
#include "order_book.hpp"
#include "sequence_tracker.hpp"

struct ReconstructionResult
{
    SequenceResult sequence;
    bool applied;
    bool synchronized;
};

class BookReconstructor
{
public:
    // First, in-order, and forward-gap snapshots establish synchronization.
    // Duplicate and stale snapshots are ignored to avoid rolling back state.
    // An update gap disables update application until an accepted snapshot.
    ReconstructionResult process(
        const ParsedBookMessage& message,
        const SequenceResult& sequence
    );

    void mark_desynchronized() noexcept;

    const OrderBook& book() const noexcept;
    bool synchronized() const noexcept;

private:
    void apply_updates(const ParsedBookMessage& message);

    OrderBook book_;
    bool synchronized_ = false;
};
