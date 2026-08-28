#include "book_reconstructor.hpp"
#include "sequence_tracker.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace
{
ParsedBookMessage make_message(
    std::uint64_t sequence_num,
    BookEventType type,
    std::vector<BookUpdate> updates)
{
    return {sequence_num, type, std::move(updates)};
}
}

TEST(SequenceTrackerTest, FirstSequence)
{
    SequenceTracker tracker;

    const SequenceResult result = tracker.observe(10);

    EXPECT_EQ(result.status, SequenceStatus::First);
    EXPECT_EQ(result.expected, 10U);
    EXPECT_EQ(result.received, 10U);
    EXPECT_TRUE(tracker.initialized());
}

TEST(SequenceTrackerTest, InOrderSequence)
{
    SequenceTracker tracker;
    tracker.observe(10);

    const SequenceResult result = tracker.observe(11);

    EXPECT_EQ(result.status, SequenceStatus::InOrder);
    EXPECT_EQ(result.expected, 11U);
    EXPECT_EQ(result.received, 11U);
}

TEST(SequenceTrackerTest, DuplicateSequence)
{
    SequenceTracker tracker;
    tracker.observe(10);

    const SequenceResult result = tracker.observe(10);

    EXPECT_EQ(result.status, SequenceStatus::Duplicate);
    EXPECT_EQ(result.expected, 11U);
    EXPECT_EQ(result.received, 10U);
}

TEST(SequenceTrackerTest, StaleSequence)
{
    SequenceTracker tracker;
    tracker.observe(10);

    const SequenceResult result = tracker.observe(9);

    EXPECT_EQ(result.status, SequenceStatus::Stale);
    EXPECT_EQ(result.expected, 11U);
    EXPECT_EQ(result.received, 9U);
}

TEST(SequenceTrackerTest, GapSequence)
{
    SequenceTracker tracker;
    tracker.observe(10);

    const SequenceResult result = tracker.observe(12);

    EXPECT_EQ(result.status, SequenceStatus::Gap);
    EXPECT_EQ(result.expected, 11U);
    EXPECT_EQ(result.received, 12U);
}

TEST(SequenceTrackerTest, ResetAllowsNewFirstSequence)
{
    SequenceTracker tracker;
    tracker.observe(10);
    tracker.reset();

    const SequenceResult result = tracker.observe(50);

    EXPECT_EQ(result.status, SequenceStatus::First);
    EXPECT_EQ(result.expected, 50U);
    EXPECT_EQ(result.received, 50U);
}

TEST(SequenceTrackerTest, DuplicateDoesNotAdvanceState)
{
    SequenceTracker tracker;
    tracker.observe(10);
    tracker.observe(10);

    const SequenceResult result = tracker.observe(11);

    EXPECT_EQ(result.status, SequenceStatus::InOrder);
}

TEST(SequenceTrackerTest, StaleDoesNotAdvanceState)
{
    SequenceTracker tracker;
    tracker.observe(10);
    tracker.observe(9);

    const SequenceResult result = tracker.observe(11);

    EXPECT_EQ(result.status, SequenceStatus::InOrder);
}

TEST(SequenceTrackerTest, GapBehaviorMatchesDocumentedPolicy)
{
    SequenceTracker tracker;
    tracker.observe(10);
    tracker.observe(12);

    const SequenceResult still_gapped = tracker.observe(13);
    const SequenceResult missing_sequence = tracker.observe(11);

    EXPECT_EQ(still_gapped.status, SequenceStatus::Gap);
    EXPECT_EQ(still_gapped.expected, 11U);
    EXPECT_EQ(missing_sequence.status, SequenceStatus::InOrder);
}

TEST(SequenceTrackerTest, NonBookMessageMaintainsGlobalSequenceContinuity)
{
    SequenceTracker tracker;
    tracker.observe(2);

    const ParsedCoinbaseMessage subscriptions =
        CoinbaseParser::parse_message(R"({
            "channel":"subscriptions",
            "sequence_num":3,
            "events":[{"subscriptions":{"level2":["BTC-USD"]}}]
        })");

    ASSERT_FALSE(subscriptions.book_message.has_value());
    const SequenceResult subscription_sequence =
        tracker.observe(subscriptions.sequence_num);
    const SequenceResult next_l2_sequence = tracker.observe(4);

    EXPECT_EQ(subscription_sequence.status, SequenceStatus::InOrder);
    EXPECT_EQ(next_l2_sequence.status, SequenceStatus::InOrder);
}

TEST(BookReconstructorTest, AppliesSnapshotAndInOrderUpdates)
{
    BookReconstructor reconstructor;

    SequenceTracker tracker;
    tracker.reset();
    const ParsedBookMessage snapshot_message = make_message(
        10,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}, {"offer", 105.0, 1.0}}
    );
    const auto snapshot = reconstructor.process(
        snapshot_message,
        tracker.observe(snapshot_message.sequence_num)
    );
    const ParsedBookMessage first_update_message = make_message(
        11,
        BookEventType::Update,
        {{"bid", 101.0, 2.0}}
    );
    const auto first_update = reconstructor.process(
        first_update_message,
        tracker.observe(first_update_message.sequence_num)
    );
    const ParsedBookMessage second_update_message = make_message(
        12,
        BookEventType::Update,
        {{"offer", 104.0, 3.0}}
    );
    const auto second_update = reconstructor.process(
        second_update_message,
        tracker.observe(second_update_message.sequence_num)
    );

    EXPECT_TRUE(snapshot.applied);
    EXPECT_TRUE(first_update.applied);
    EXPECT_TRUE(second_update.applied);
    EXPECT_TRUE(reconstructor.synchronized());
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    ASSERT_TRUE(reconstructor.book().best_ask().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 101.0);
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_ask(), 104.0);
}

TEST(BookReconstructorTest, GapStopsUpdatesUntilSnapshotRestoresSynchronization)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;
    tracker.reset();
    const ParsedBookMessage snapshot_message = make_message(
        10,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}, {"offer", 101.0, 1.0}}
    );
    reconstructor.process(
        snapshot_message,
        tracker.observe(snapshot_message.sequence_num)
    );

    const ParsedBookMessage gap_message = make_message(
        12,
        BookEventType::Update,
        {{"bid", 200.0, 1.0}}
    );
    const auto gap = reconstructor.process(
        gap_message,
        tracker.observe(gap_message.sequence_num)
    );
    const ParsedBookMessage late_missing_message = make_message(
        11,
        BookEventType::Update,
        {{"bid", 150.0, 1.0}}
    );
    const auto late_missing_update = reconstructor.process(
        late_missing_message,
        tracker.observe(late_missing_message.sequence_num)
    );

    EXPECT_EQ(gap.sequence.status, SequenceStatus::Gap);
    EXPECT_FALSE(gap.applied);
    EXPECT_FALSE(late_missing_update.applied);
    EXPECT_FALSE(reconstructor.synchronized());
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 100.0);

    const ParsedBookMessage recovery_message = make_message(
        20,
        BookEventType::Snapshot,
        {{"bid", 300.0, 1.0}, {"offer", 301.0, 1.0}}
    );
    const SequenceResult recovery_sequence =
        tracker.observe(recovery_message.sequence_num);
    const auto recovery = reconstructor.process(
        recovery_message,
        recovery_sequence
    );

    if (recovery.applied)
    {
        tracker.reset();
        tracker.observe(recovery_message.sequence_num);
    }

    EXPECT_EQ(recovery.sequence.status, SequenceStatus::Gap);
    EXPECT_TRUE(recovery.applied);
    EXPECT_TRUE(reconstructor.synchronized());
    EXPECT_EQ(reconstructor.book().bid_levels(), 1U);
    EXPECT_EQ(reconstructor.book().ask_levels(), 1U);
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 300.0);
    EXPECT_EQ(tracker.observe(21).status, SequenceStatus::InOrder);
}

TEST(BookReconstructorTest, GapSnapshotRepairsSynchronizationAndReanchors)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;

    const ParsedBookMessage initial_snapshot = make_message(
        100,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}, {"offer", 101.0, 1.0}}
    );
    reconstructor.process(
        initial_snapshot,
        tracker.observe(initial_snapshot.sequence_num)
    );

    const ParsedBookMessage update = make_message(
        101,
        BookEventType::Update,
        {{"bid", 100.5, 1.0}}
    );
    reconstructor.process(
        update,
        tracker.observe(update.sequence_num)
    );

    const ParsedBookMessage recovery_snapshot = make_message(
        105,
        BookEventType::Snapshot,
        {{"bid", 105.0, 1.0}, {"offer", 106.0, 1.0}}
    );
    const SequenceResult gap = tracker.observe(
        recovery_snapshot.sequence_num
    );
    const ReconstructionResult recovery = reconstructor.process(
        recovery_snapshot,
        gap
    );

    ASSERT_EQ(gap.status, SequenceStatus::Gap);
    EXPECT_EQ(gap.expected, 102U);
    EXPECT_EQ(gap.received, 105U);
    ASSERT_TRUE(recovery.applied);
    EXPECT_TRUE(reconstructor.synchronized());
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 105.0);

    tracker.reset();
    tracker.observe(recovery_snapshot.sequence_num);

    EXPECT_EQ(tracker.observe(106).status, SequenceStatus::InOrder);
}

TEST(BookReconstructorTest, DuplicateSnapshotDoesNotReplaceCurrentBook)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;

    const ParsedBookMessage initial_snapshot = make_message(
        100,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}}
    );
    reconstructor.process(
        initial_snapshot,
        tracker.observe(initial_snapshot.sequence_num)
    );

    const ParsedBookMessage update = make_message(
        101,
        BookEventType::Update,
        {{"bid", 101.0, 1.0}}
    );
    reconstructor.process(
        update,
        tracker.observe(update.sequence_num)
    );

    const ParsedBookMessage duplicate_snapshot = make_message(
        101,
        BookEventType::Snapshot,
        {{"bid", 50.0, 1.0}}
    );
    const ReconstructionResult duplicate = reconstructor.process(
        duplicate_snapshot,
        tracker.observe(duplicate_snapshot.sequence_num)
    );

    EXPECT_EQ(duplicate.sequence.status, SequenceStatus::Duplicate);
    EXPECT_FALSE(duplicate.applied);
    EXPECT_TRUE(reconstructor.synchronized());
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 101.0);
}

TEST(BookReconstructorTest, StaleSnapshotDoesNotReplaceCurrentBook)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;

    const ParsedBookMessage initial_snapshot = make_message(
        100,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}}
    );
    reconstructor.process(
        initial_snapshot,
        tracker.observe(initial_snapshot.sequence_num)
    );

    const ParsedBookMessage first_update = make_message(
        101,
        BookEventType::Update,
        {{"bid", 101.0, 1.0}}
    );
    reconstructor.process(
        first_update,
        tracker.observe(first_update.sequence_num)
    );

    const ParsedBookMessage second_update = make_message(
        102,
        BookEventType::Update,
        {{"bid", 102.0, 1.0}}
    );
    reconstructor.process(
        second_update,
        tracker.observe(second_update.sequence_num)
    );

    const ParsedBookMessage stale_snapshot = make_message(
        101,
        BookEventType::Snapshot,
        {{"bid", 50.0, 1.0}}
    );
    const ReconstructionResult stale = reconstructor.process(
        stale_snapshot,
        tracker.observe(stale_snapshot.sequence_num)
    );

    EXPECT_EQ(stale.sequence.status, SequenceStatus::Stale);
    EXPECT_FALSE(stale.applied);
    EXPECT_TRUE(reconstructor.synchronized());
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 102.0);
}

TEST(BookReconstructorTest, DuplicateMessageDoesNotMutateBook)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;
    tracker.reset();
    const ParsedBookMessage snapshot_message = make_message(
        10,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}}
    );
    reconstructor.process(
        snapshot_message,
        tracker.observe(snapshot_message.sequence_num)
    );
    const ParsedBookMessage update_message = make_message(
        11,
        BookEventType::Update,
        {{"bid", 101.0, 1.0}}
    );
    reconstructor.process(
        update_message,
        tracker.observe(update_message.sequence_num)
    );

    const ParsedBookMessage duplicate_message = make_message(
        11,
        BookEventType::Update,
        {{"bid", 200.0, 1.0}}
    );
    const auto duplicate = reconstructor.process(
        duplicate_message,
        tracker.observe(duplicate_message.sequence_num)
    );

    EXPECT_EQ(duplicate.sequence.status, SequenceStatus::Duplicate);
    EXPECT_FALSE(duplicate.applied);
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 101.0);
}

TEST(BookReconstructorTest, StaleMessageDoesNotMutateBook)
{
    BookReconstructor reconstructor;
    SequenceTracker tracker;
    tracker.reset();
    const ParsedBookMessage snapshot_message = make_message(
        10,
        BookEventType::Snapshot,
        {{"bid", 100.0, 1.0}}
    );
    reconstructor.process(
        snapshot_message,
        tracker.observe(snapshot_message.sequence_num)
    );

    const ParsedBookMessage stale_message = make_message(
        9,
        BookEventType::Update,
        {{"bid", 200.0, 1.0}}
    );
    const auto stale = reconstructor.process(
        stale_message,
        tracker.observe(stale_message.sequence_num)
    );

    EXPECT_EQ(stale.sequence.status, SequenceStatus::Stale);
    EXPECT_FALSE(stale.applied);
    ASSERT_TRUE(reconstructor.book().best_bid().has_value());
    EXPECT_DOUBLE_EQ(*reconstructor.book().best_bid(), 100.0);
}
