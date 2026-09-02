#include "market.hpp"

#include <algorithm>
#include <stdexcept>

namespace
{
bool is_ascii_space(char character) noexcept
{
    return character == ' '
        || character == '\t'
        || character == '\n'
        || character == '\r'
        || character == '\f'
        || character == '\v';
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && is_ascii_space(text.front()))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && is_ascii_space(text.back()))
    {
        text.remove_suffix(1);
    }

    return text;
}

bool is_ascii_alphanumeric(char character) noexcept
{
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9');
}

char ascii_upper(char character) noexcept
{
    return character >= 'a' && character <= 'z'
        ? static_cast<char>(character - 'a' + 'A')
        : character;
}

char ascii_lower(char character) noexcept
{
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character - 'A' + 'a')
        : character;
}

std::string normalize_component(std::string_view component)
{
    component = trim(component);

    if (component.empty() || component.size() > 32)
    {
        throw std::invalid_argument(
            "Product component must contain between 1 and 32 characters"
        );
    }

    std::string normalized(component);

    for (char& character : normalized)
    {
        if (!is_ascii_alphanumeric(character))
        {
            throw std::invalid_argument(
                "Product components may contain only ASCII letters and digits"
            );
        }

        character = ascii_upper(character);
    }

    return normalized;
}

std::size_t combine_hash(std::size_t left, std::size_t right) noexcept
{
    return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
}
}

Product::Product(std::string_view base, std::string_view quote)
    : base_(normalize_component(base)),
      quote_(normalize_component(quote))
{
}

Product Product::parse(std::string_view text)
{
    text = trim(text);
    const std::size_t separator = text.find('-');

    if (
        separator == std::string_view::npos
        || text.find('-', separator + 1) != std::string_view::npos
    )
    {
        throw std::invalid_argument("Product must be a BASE-QUOTE pair");
    }

    return Product(text.substr(0, separator), text.substr(separator + 1));
}

const std::string& Product::base() const noexcept
{
    return base_;
}

const std::string& Product::quote() const noexcept
{
    return quote_;
}

std::string Product::to_string() const
{
    return base_ + '-' + quote_;
}

std::string_view venue_name(Venue venue)
{
    switch (venue)
    {
        case Venue::Coinbase:
            return "coinbase";
        case Venue::Kraken:
            return "kraken";
    }

    throw std::invalid_argument("Unsupported venue");
}

Venue parse_venue(std::string_view text)
{
    text = trim(text);
    std::string normalized(text);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        ascii_lower
    );

    if (normalized == "coinbase")
    {
        return Venue::Coinbase;
    }

    if (normalized == "kraken")
    {
        return Venue::Kraken;
    }

    throw std::invalid_argument("Venue must be coinbase or kraken");
}

std::size_t std::hash<Product>::operator()(
    const Product& product) const noexcept
{
    return combine_hash(
        std::hash<std::string>{}(product.base()),
        std::hash<std::string>{}(product.quote())
    );
}

std::size_t std::hash<MarketKey>::operator()(
    const MarketKey& market) const noexcept
{
    return combine_hash(
        std::hash<int>{}(static_cast<int>(market.venue)),
        std::hash<Product>{}(market.product)
    );
}
