#include "coinbase_level2_stream.hpp"

#include "recorder_config.hpp"

#include <stdexcept>
#include <string>
#include <utility>

CoinbaseLevel2Stream::CoinbaseLevel2Stream(std::string_view product_id)
    : CoinbaseLevel2Stream(product_id, make_coinbase_wire())
{
}

CoinbaseLevel2Stream::CoinbaseLevel2Stream(
    std::string_view product_id,
    std::unique_ptr<CoinbaseWire> wire)
    : wire_(std::move(wire))
{
    if (!wire_)
    {
        throw std::invalid_argument("Coinbase stream requires a wire");
    }

    wire_->write(make_level2_subscription(product_id));
    wire_->write(make_heartbeat_subscription());
}

CoinbaseLevel2Stream::~CoinbaseLevel2Stream() = default;
CoinbaseLevel2Stream::CoinbaseLevel2Stream(
    CoinbaseLevel2Stream&&) noexcept = default;
CoinbaseLevel2Stream& CoinbaseLevel2Stream::operator=(
    CoinbaseLevel2Stream&&) noexcept = default;

std::string CoinbaseLevel2Stream::read()
{
    return wire_->read();
}
