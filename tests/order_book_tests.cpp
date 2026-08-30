#include "order_book.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

TEST(OrderBookTest, EmptyBookHasNoBestBid)
{
    const OrderBook book;

    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBookTest, EmptyBookHasNoBestAsk)
{
    const OrderBook book;

    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBookTest, InsertBid)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.25, 2.0);

    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.bid_levels(), 1U);
    EXPECT_DOUBLE_EQ(*book.best_bid(), 100.25);
}

TEST(OrderBookTest, InsertAsk)
{
    OrderBook book;
    book.apply_update(BookSide::Offer, 100.75, 3.0);

    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(book.ask_levels(), 1U);
    EXPECT_DOUBLE_EQ(*book.best_ask(), 100.75);
}

TEST(OrderBookTest, HighestBidWins)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 99.0, 1.0);
    book.apply_update(BookSide::Bid, 101.0, 1.0);
    book.apply_update(BookSide::Bid, 100.0, 1.0);

    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_DOUBLE_EQ(*book.best_bid(), 101.0);
}

TEST(OrderBookTest, LowestAskWins)
{
    OrderBook book;
    book.apply_update(BookSide::Offer, 102.0, 1.0);
    book.apply_update(BookSide::Offer, 100.0, 1.0);
    book.apply_update(BookSide::Offer, 101.0, 1.0);

    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_DOUBLE_EQ(*book.best_ask(), 100.0);
}

TEST(OrderBookTest, UpdateExistingLevel)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.0, 1.0);
    book.apply_update(BookSide::Bid, 100.0, 5.0);

    EXPECT_EQ(book.bid_levels(), 1U);
}

TEST(OrderBookTest, ZeroQuantityDeletesBid)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.0, 1.0);
    book.apply_update(BookSide::Bid, 100.0, 0.0);

    EXPECT_EQ(book.bid_levels(), 0U);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBookTest, ZeroQuantityDeletesAsk)
{
    OrderBook book;
    book.apply_update(BookSide::Offer, 101.0, 1.0);
    book.apply_update(BookSide::Offer, 101.0, 0.0);

    EXPECT_EQ(book.ask_levels(), 0U);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBookTest, SpreadIsCorrect)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.25, 1.0);
    book.apply_update(BookSide::Offer, 100.75, 1.0);

    ASSERT_TRUE(book.spread().has_value());
    EXPECT_DOUBLE_EQ(*book.spread(), 0.5);
}

TEST(OrderBookTest, SpreadUnavailableWhenOneSideMissing)
{
    OrderBook bid_only_book;
    bid_only_book.apply_update(BookSide::Bid, 100.0, 1.0);

    OrderBook ask_only_book;
    ask_only_book.apply_update(BookSide::Offer, 101.0, 1.0);

    EXPECT_FALSE(bid_only_book.spread().has_value());
    EXPECT_FALSE(ask_only_book.spread().has_value());
}

TEST(OrderBookTest, ClearRemovesBothSides)
{
    OrderBook book;
    book.apply_update(BookSide::Bid, 100.0, 1.0);
    book.apply_update(BookSide::Offer, 101.0, 1.0);

    book.clear();

    EXPECT_EQ(book.bid_levels(), 0U);
    EXPECT_EQ(book.ask_levels(), 0U);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.spread().has_value());
}

TEST(OrderBookTest, UnknownSideThrowsInvalidArgument)
{
    OrderBook book;

    EXPECT_THROW(
        book.apply_update(static_cast<BookSide>(255), 100.0, 1.0),
        std::invalid_argument
    );
}
