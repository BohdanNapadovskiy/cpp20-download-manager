#pragma once
#include "core/http_transfer.hpp"
#include <chrono>
#include <cstdint>

namespace dm::core {
    struct RetryPolicy {
        std::uint32_t max_attempts = 5;
        std::chrono::milliseconds initial_delay = std::chrono::seconds(1);
        std::chrono::milliseconds max_delay = std::chrono::seconds(30);
    };
    [[nodiscard]] bool should_retry(const TransferResult& result);
    // `attempt` is the number of the attempt about to start, counting from 1: the first attempt
    // starts at once (0), the second waits initial_delay, each next one twice as long as the
    // previous, never longer than max_delay. Defaults: 0, 1 s, 2 s, 4 s, 8 s, ...
    [[nodiscard]] std::chrono::milliseconds retry_delay(const RetryPolicy& policy, std::uint32_t attempt);
}
