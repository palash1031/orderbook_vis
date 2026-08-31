#pragma once

#include <string>

class LiveMessageSource
{
public:
    virtual ~LiveMessageSource() = default;

    virtual std::string read() = 0;
};
