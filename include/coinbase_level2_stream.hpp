#pragma once

#include <memory>
#include <string>
#include <string_view>

class CoinbaseLevel2Stream
{
public:
    explicit CoinbaseLevel2Stream(std::string_view product_id);
    ~CoinbaseLevel2Stream();

    CoinbaseLevel2Stream(const CoinbaseLevel2Stream&) = delete;
    CoinbaseLevel2Stream& operator=(const CoinbaseLevel2Stream&) = delete;
    CoinbaseLevel2Stream(CoinbaseLevel2Stream&&) noexcept;
    CoinbaseLevel2Stream& operator=(CoinbaseLevel2Stream&&) noexcept;

    std::string read();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
