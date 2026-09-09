#pragma once

#include "consolidated_quote.hpp"
#include "universe.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class ScannerStreamSubscriber
{
public:
    std::optional<std::string> wait_for_message(
        std::chrono::milliseconds timeout
    );

private:
    friend class ScannerStreamHub;

    void enqueue(std::string message);
    void replace_queue(std::vector<std::string> messages);

    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<std::string> messages_;
};

class ScannerStreamHub
{
public:
    explicit ScannerStreamHub(MarketUniverse universe);

    std::shared_ptr<ScannerStreamSubscriber> subscribe();
    void publish(const ConsolidatedQuote& quote);

private:
    std::vector<std::string> snapshot_messages() const;
    void remove_expired_subscribers();
    void enqueue_to_all(const std::string& message);

    MarketUniverse universe_;
    std::string hello_message_;
    std::map<Product, std::string> current_updates_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<ScannerStreamSubscriber>> subscribers_;
};
