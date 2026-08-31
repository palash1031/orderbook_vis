#pragma once

#include "live_message_source.hpp"

#include <memory>
#include <string>
#include <string_view>

class CoinbaseLevel2Stream final : public LiveMessageSource
{
public:
    explicit CoinbaseLevel2Stream(std::string_view product_id);
    ~CoinbaseLevel2Stream() override;

    CoinbaseLevel2Stream(const CoinbaseLevel2Stream&) = delete;
    CoinbaseLevel2Stream& operator=(const CoinbaseLevel2Stream&) = delete;
    CoinbaseLevel2Stream(CoinbaseLevel2Stream&&) noexcept;
    CoinbaseLevel2Stream& operator=(CoinbaseLevel2Stream&&) noexcept;

    std::string read() override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
