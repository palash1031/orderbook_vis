#include "live_source.hpp"

#include "recorder_config.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace json = boost::json;

namespace
{
std::optional<std::string> coinbase_error_message(
    std::string_view raw_message)
{
    try
    {
        const json::object& message = json::parse(raw_message).as_object();
        const auto* type = message.if_contains("type");
        const auto* channel = message.if_contains("channel");
        const bool is_error =
            (type && type->is_string() && type->as_string() == "error")
            || (
                channel
                && channel->is_string()
                && channel->as_string() == "error"
            );

        if (!is_error)
        {
            return std::nullopt;
        }

        for (const char* field : {"message", "error", "reason"})
        {
            const auto* value = message.if_contains(field);

            if (value && value->is_string())
            {
                const auto& text = value->as_string();
                return std::string(text.c_str(), text.size());
            }
        }

        return std::string("Coinbase rejected the live subscription");
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}
}

LiveSourceRunner::LiveSourceRunner(
    std::string_view product_id,
    HeatmapConfig heatmap_config,
    std::shared_ptr<LiveStreamHub> hub,
    LiveMessageSourceFactory source_factory,
    ReconnectBackoffConfig backoff_config,
    LiveSourceSleeper sleeper)
    : product_id_(normalize_product_id(product_id)),
      engine_(product_id_, std::move(heatmap_config)),
      hub_(std::move(hub)),
      source_factory_(std::move(source_factory)),
      sleeper_(std::move(sleeper)),
      backoff_(backoff_config)
{
    if (!hub_ || !source_factory_)
    {
        throw std::invalid_argument(
            "Live source requires a stream hub and source factory"
        );
    }

    if (!sleeper_)
    {
        sleeper_ = [](std::chrono::milliseconds delay)
        {
            std::this_thread::sleep_for(delay);
        };
    }
}

void LiveSourceRunner::run(LiveSourceStopCheck should_stop)
{
    if (!should_stop)
    {
        should_stop = []
        {
            return false;
        };
    }

    hub_->publish_source_status(LiveSourceStatus::Connecting);

    while (!should_stop())
    {
        try
        {
            std::unique_ptr<LiveMessageSource> source = source_factory_();

            if (!source)
            {
                throw std::runtime_error(
                    "Live message source factory returned no connection"
                );
            }

            hub_->publish_source_status(LiveSourceStatus::Connected);
            std::cout
                << "Coinbase live source connected for "
                << product_id_
                << '\n';

            while (!should_stop())
            {
                const std::string message = source->read();

                if (const auto error = coinbase_error_message(message))
                {
                    hub_->publish_source_status(
                        LiveSourceStatus::Disconnected,
                        *error
                    );
                    hub_->publish_error(*error);
                    std::cerr
                        << "Coinbase subscription error: "
                        << *error
                        << '\n';
                    return;
                }

                try
                {
                    const LiveHeatmapResult result = engine_.process(message);
                    hub_->publish(engine_, result);

                    if (engine_.status() == LiveHeatmapStatus::Live)
                    {
                        backoff_.reset();
                    }

                    if (engine_.status() == LiveHeatmapStatus::Gap)
                    {
                        throw std::runtime_error(
                            "Coinbase Level 2 sequence gap detected"
                        );
                    }
                }
                catch (const std::invalid_argument& error)
                {
                    std::cerr
                        << "Coinbase live message ignored: "
                        << error.what()
                        << '\n';
                }
            }

            return;
        }
        catch (const std::exception& error)
        {
            if (should_stop())
            {
                return;
            }

            const std::string detail =
                std::string("Coinbase live source disconnected: ")
                + error.what();
            hub_->publish_source_status(
                LiveSourceStatus::Disconnected,
                detail
            );
            hub_->publish(engine_, engine_.begin_recovery());
            const std::chrono::milliseconds retry_delay =
                backoff_.next_delay();
            hub_->publish_source_status(
                LiveSourceStatus::Reconnecting,
                detail,
                retry_delay
            );
            std::cerr
                << detail
                << "; retrying in "
                << retry_delay.count()
                << " ms\n";
            sleeper_(retry_delay);
            hub_->publish_source_status(
                LiveSourceStatus::Reconnecting,
                "Opening a new Coinbase connection"
            );
        }
    }
}
