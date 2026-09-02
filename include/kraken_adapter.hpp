#pragma once

#include "market.hpp"
#include "venue_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

inline constexpr std::size_t default_kraken_book_depth = 500;

bool is_supported_kraken_book_depth(std::size_t depth) noexcept;
std::string make_kraken_instrument_subscription();
std::string make_kraken_book_subscription(
    std::string_view native_symbol,
    std::size_t depth = default_kraken_book_depth
);

class KrakenInstrumentCatalog
{
public:
    static KrakenInstrumentCatalog parse(std::string_view raw_message);

    std::optional<std::string> native_symbol(const Product& product) const;
    std::optional<Product> canonical_product(
        std::string_view native_symbol
    ) const;

private:
    std::map<Product, std::string> native_by_product_;
    std::map<std::string, Product> product_by_native_;
};

class KrakenBookAdapter final : public VenueAdapter
{
public:
    KrakenBookAdapter(
        Product product,
        std::string_view native_symbol,
        std::size_t depth = default_kraken_book_depth
    );

    std::optional<TrustedBookEvent> process(
        std::string_view raw_message
    ) override;
    void reset() noexcept override;
    VenueMarketStatus status() const noexcept override;

    std::uint32_t checksum() const;
    std::string checksum_input() const;

private:
    struct WireLevel
    {
        std::string price_text;
        std::string quantity_text;
    };

    struct ExactDecimalLess
    {
        bool operator()(
            const std::string& left,
            const std::string& right
        ) const noexcept;
    };

    using Levels = std::map<std::string, WireLevel, ExactDecimalLess>;

    static std::string checksum_input_for(
        const Levels& bids,
        const Levels& asks
    );
    static void truncate(Levels& bids, Levels& asks, std::size_t depth);

    TrustedBookEvent invalidation() noexcept;
    static OrderBook numeric_book_for(
        const Levels& bids,
        const Levels& asks
    );

    Product product_;
    std::string native_symbol_;
    std::size_t depth_;
    Levels bids_;
    Levels asks_;
    VenueMarketStatus status_ = VenueMarketStatus::WaitingForSnapshot;
    MarketTimestamp last_timestamp_{};
};
