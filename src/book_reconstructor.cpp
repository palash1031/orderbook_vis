#include "book_reconstructor.hpp"

ReconstructionResult BookReconstructor::process(
    const ParsedBookMessage& message,
    const SequenceResult& sequence)
{
    if (message.type == BookEventType::Snapshot)
    {
        if (
            sequence.status == SequenceStatus::Duplicate
            || sequence.status == SequenceStatus::Stale
        )
        {
            return {sequence, false, synchronized_};
        }

        book_.clear();
        apply_updates(message);
        synchronized_ = true;

        return {sequence, true, synchronized_};
    }

    if (sequence.status == SequenceStatus::Gap)
    {
        synchronized_ = false;
        return {sequence, false, synchronized_};
    }

    if (sequence.status != SequenceStatus::InOrder || !synchronized_)
    {
        return {sequence, false, synchronized_};
    }

    apply_updates(message);
    return {sequence, true, synchronized_};
}

void BookReconstructor::mark_desynchronized() noexcept
{
    synchronized_ = false;
}

const OrderBook& BookReconstructor::book() const noexcept
{
    return book_;
}

bool BookReconstructor::synchronized() const noexcept
{
    return synchronized_;
}

void BookReconstructor::apply_updates(
    const ParsedBookMessage& message)
{
    for (const auto& update : message.updates)
    {
        book_.apply_update(
            update.side,
            update.price,
            update.quantity
        );
    }
}
