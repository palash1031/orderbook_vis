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
        const json::object message = json::parse(raw_message).as_object();
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
    LiveSourceSleeper sleeper,
    std::string_view source_name)
    : LiveSourceRunner(
          0,
          product_id,
          std::move(heatmap_config),
          std::move(hub),
          std::move(source_factory),
          backoff_config,
          std::move(sleeper),
          source_name
      )
{
}

LiveSourceRunner::LiveSourceRunner(
    LiveStreamSessionId session_id,
    std::string_view product_id,
    HeatmapConfig heatmap_config,
    std::shared_ptr<LiveStreamHub> hub,
    LiveMessageSourceFactory source_factory,
    ReconnectBackoffConfig backoff_config,
    LiveSourceSleeper sleeper,
    std::string_view source_name)
    : session_id_(session_id),
      product_id_(normalize_product_id(product_id)),
      engine_(product_id_, std::move(heatmap_config)),
      hub_(std::move(hub)),
      source_factory_(std::move(source_factory)),
      sleeper_(std::move(sleeper)),
      backoff_(backoff_config),
      source_name_(source_name)
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

    hub_->publish_source_status(
        session_id_,
        LiveSourceStatus::Connecting
    );

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

            hub_->publish_source_status(
                session_id_,
                LiveSourceStatus::Connected
            );
            std::cout
                << source_name_
                << " live source connected for "
                << product_id_
                << '\n';

            while (!should_stop())
            {
                try
                {
                    LiveHeatmapResult result;

                    if (auto* trusted = dynamic_cast<TrustedLiveMessageSource*>(
                            source.get()
                        ))
                    {
                        const TrustedBookEvent event = trusted->read_event();

                        if (should_stop())
                        {
                            return;
                        }

                        result = engine_.process(event);
                        hub_->publish(session_id_, engine_, result);

                        if (event.type == TrustedBookEventType::Invalidated)
                        {
                            throw std::runtime_error(
                                "trusted order book invalidated"
                            );
                        }
                    }
                    else
                    {
                        const std::string message = source->read();

                        if (should_stop())
                        {
                            return;
                        }

                        if (const auto error = coinbase_error_message(message))
                        {
                            hub_->publish_source_status(
                                session_id_,
                                LiveSourceStatus::Disconnected,
                                *error
                            );
                            hub_->publish_error(session_id_, *error);
                            std::cerr
                                << "Coinbase subscription error: "
                                << *error
                                << '\n';
                            return;
                        }

                        result = engine_.process(message);
                        hub_->publish(session_id_, engine_, result);
                    }

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
                        << source_name_
                        << " live message ignored: "
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
                source_name_
                + " live source disconnected: "
                + error.what();
            hub_->publish_source_status(
                session_id_,
                LiveSourceStatus::Disconnected,
                detail
            );
            hub_->publish(
                session_id_,
                engine_,
                engine_.begin_recovery()
            );
            const std::chrono::milliseconds retry_delay =
                backoff_.next_delay();
            hub_->publish_source_status(
                session_id_,
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

            if (should_stop())
            {
                return;
            }

            hub_->publish_source_status(
                session_id_,
                LiveSourceStatus::Reconnecting,
                "Opening a new " + source_name_ + " connection"
            );
        }
    }
}

LiveMarketService::LiveMarketService(
    std::string_view initial_product_id,
    HeatmapConfig heatmap_config,
    std::shared_ptr<LiveStreamHub> hub,
    LiveProductSourceFactory source_factory,
    ReconnectBackoffConfig backoff_config,
    LiveSourceSleeper sleeper,
    std::string_view source_name)
    : heatmap_config_(std::move(heatmap_config)),
      hub_(std::move(hub)),
      source_factory_(std::move(source_factory)),
      backoff_config_(backoff_config),
      sleeper_(std::move(sleeper)),
      source_name_(source_name)
{
    if (!hub_ || !source_factory_)
    {
        throw std::invalid_argument(
            "Live market service requires a stream hub and source factory"
        );
    }

    selection_.product_id = normalize_product_id(initial_product_id);
    selection_.session_id = hub_->begin_product_session(
        selection_.product_id
    );
}

void LiveMarketService::run(LiveSourceStopCheck should_stop)
{
    if (!should_stop)
    {
        should_stop = []
        {
            return false;
        };
    }

    while (!should_stop())
    {
        Selection selected;

        {
            std::lock_guard lock(mutex_);
            selected = selection_;
        }

        LiveSourceRunner runner(
            selected.session_id,
            selected.product_id,
            heatmap_config_,
            hub_,
            [this, product_id = selected.product_id]
            {
                return source_factory_(product_id);
            },
            backoff_config_,
            sleeper_,
            source_name_
        );
        runner.run([this, should_stop, session_id = selected.session_id]
        {
            return should_stop() || !is_current_session(session_id);
        });

        if (should_stop())
        {
            return;
        }

        std::unique_lock lock(mutex_);
        while (
            selection_.session_id == selected.session_id
            && !should_stop()
        )
        {
            changed_.wait_for(lock, std::chrono::milliseconds{50});
        }
    }
}

bool LiveMarketService::switch_product(std::string_view product_id)
{
    const std::string normalized = normalize_product_id(product_id);
    std::lock_guard lock(mutex_);

    if (normalized == selection_.product_id)
    {
        return false;
    }

    selection_.product_id = normalized;
    selection_.session_id = hub_->begin_product_session(normalized);
    changed_.notify_all();
    return true;
}

void LiveMarketService::apply_control(std::string_view command_json)
{
    try
    {
        const json::object command = json::parse(command_json).as_object();
        const auto& action = command.at("action").as_string();

        if (action != "switch_product")
        {
            throw std::invalid_argument("Unsupported live control action");
        }

        const auto& product = command.at("product_id").as_string();
        switch_product(std::string_view(product.c_str(), product.size()));
    }
    catch (const std::invalid_argument&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string("Invalid live control: ") + error.what()
        );
    }
}

std::string LiveMarketService::product_id() const
{
    std::lock_guard lock(mutex_);
    return selection_.product_id;
}

bool LiveMarketService::is_current_session(
    LiveStreamSessionId session_id) const
{
    std::lock_guard lock(mutex_);
    return selection_.session_id == session_id;
}
