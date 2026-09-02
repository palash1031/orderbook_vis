#include "live_message_source.hpp"

#include <stdexcept>

std::string TrustedLiveMessageSource::read()
{
    throw std::logic_error(
        "Trusted live message sources must be read as trusted events"
    );
}
