#pragma once

#include "venue_adapter.hpp"

#include <string>

class LiveMessageSource
{
public:
    virtual ~LiveMessageSource() = default;

    virtual std::string read() = 0;
};

class TrustedLiveMessageSource : public LiveMessageSource
{
public:
    std::string read() final;
    virtual TrustedBookEvent read_event() = 0;
};
