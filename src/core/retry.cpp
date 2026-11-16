#include "core/retry.hpp"
#include <algorithm>

namespace dm::core {
    bool should_retry(const TransferResult& result) {
        if (result.error == TransferError::Network) {
            return true;
        }
        if (result.error == TransferError::HttpStatus) {
            if (result.http_status_code >= 500 && result.http_status_code <= 599) {
                return true;
            }
            if (result.http_status_code == 408 || result.http_status_code == 429) {
                return true;
            }
        }
        return false;
    }

    std::chrono::milliseconds retry_delay(const RetryPolicy& policy, std::uint32_t attempt) {
        if (attempt <= 1 || policy.initial_delay <= std::chrono::milliseconds::zero()) {
            return std::chrono::milliseconds::zero();
        }
        std::chrono::milliseconds delay = std::min(policy.initial_delay, policy.max_delay);
        const std::uint32_t doublings = attempt - 2;
        for (std::uint32_t i = 0; i < doublings; ++i) {
            if (delay >= policy.max_delay / 2) {
                return policy.max_delay;
            }
            delay *= 2;
        }
        return std::min(delay, policy.max_delay);
    }
}