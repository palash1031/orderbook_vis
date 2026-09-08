#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class CoinbaseWire
{
public:
    virtual ~CoinbaseWire() = default;

    virtual void write(std::string_view message) = 0;
    virtual std::string read() = 0;
};

using CoinbaseWireFactory =
    std::function<std::unique_ptr<CoinbaseWire>()>;

std::unique_ptr<CoinbaseWire> make_coinbase_wire();
