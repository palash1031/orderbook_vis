#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

class Product
{
public:
    Product(std::string_view base, std::string_view quote);

    static Product parse(std::string_view text);

    const std::string& base() const noexcept;
    const std::string& quote() const noexcept;
    std::string to_string() const;

    auto operator<=>(const Product&) const = default;

private:
    std::string base_;
    std::string quote_;
};

enum class Venue
{
    Coinbase,
    Kraken
};

std::string_view venue_name(Venue venue);
Venue parse_venue(std::string_view text);

struct MarketKey
{
    Venue venue;
    Product product;

    auto operator<=>(const MarketKey&) const = default;
};

namespace std
{
template <>
struct hash<Product>
{
    std::size_t operator()(const Product& product) const noexcept;
};

template <>
struct hash<MarketKey>
{
    std::size_t operator()(const MarketKey& market) const noexcept;
};
}
