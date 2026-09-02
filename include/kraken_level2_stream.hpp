#pragma once

#include "kraken_adapter.hpp"
#include "live_message_source.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

class KrakenWire
{
public:
    virtual ~KrakenWire() = default;

    virtual void write(std::string_view message) = 0;
    virtual std::string read() = 0;
};

class KrakenLevel2Stream final : public TrustedLiveMessageSource
{
public:
    explicit KrakenLevel2Stream(
        std::string_view product_id,
        std::size_t depth = default_kraken_book_depth
    );
    KrakenLevel2Stream(
        std::string_view product_id,
        std::unique_ptr<KrakenWire> wire,
        std::size_t depth = default_kraken_book_depth
    );
    KrakenLevel2Stream(
        Product product,
        std::size_t depth,
        std::unique_ptr<KrakenWire> wire
    );
    ~KrakenLevel2Stream() override;

    KrakenLevel2Stream(const KrakenLevel2Stream&) = delete;
    KrakenLevel2Stream& operator=(const KrakenLevel2Stream&) = delete;
    KrakenLevel2Stream(KrakenLevel2Stream&&) noexcept;
    KrakenLevel2Stream& operator=(KrakenLevel2Stream&&) noexcept;

    TrustedBookEvent read_event() override;
    VenueMarketStatus status() const noexcept;

private:
    void initialize();

    Product product_;
    std::size_t depth_;
    std::unique_ptr<KrakenWire> wire_;
    std::unique_ptr<KrakenBookAdapter> adapter_;
};
