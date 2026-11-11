#pragma once

#include "core/http_transfer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace dm::test {
    // Stand-in for download_once, controlled by the test. No network, no sleeps:
    //   * block()/release() hold every call at the start until the test lets it go;
    //   * wait_started(n) waits until n calls have begun;
    //   * the outcome depends on the URL: ".../fail" -> Network error, ".../404" -> HttpStatus,
    //     anything else -> 200 and the file content "content of <url>" written to part_path.
    // Records peak concurrency, every request and the order in which calls started.
    class FakeExecutor {
    public:
        // The manager stores a copy of its executor; the copy forwards to this object.
        auto executor() {
            return [this](const core::TransferRequest& request) { return run(request); };
        }

        void block() {
            std::scoped_lock lock(mutex_);
            blocked_ = true;
        }

        void release() {
            {
                std::scoped_lock lock(mutex_);
                blocked_ = false;
            }
            cv_.notify_all();
        }

        // False if fewer than `count` calls started within the timeout.
        bool wait_started(std::size_t count, std::chrono::seconds timeout = std::chrono::seconds(5)) {
            std::unique_lock lock(mutex_);
            return cv_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
        }

        std::size_t max_active() const {
            std::scoped_lock lock(mutex_);
            return max_active_;
        }

        std::vector<core::TransferRequest> requests() const {
            std::scoped_lock lock(mutex_);
            return requests_;
        }

        static std::string content_for(const std::string& url) { return "content of " + url; }

    private:
        core::TransferResult run(const core::TransferRequest& request) {
            {
                std::unique_lock lock(mutex_);
                requests_.push_back(request);
                ++active_;
                max_active_ = std::max(max_active_, active_);
                cv_.notify_all();
                cv_.wait(lock, [&] { return !blocked_; });
            }

            core::TransferResult result = outcome(request);

            {
                std::scoped_lock lock(mutex_);
                --active_;
            }
            return result;
        }

        static core::TransferResult outcome(const core::TransferRequest& request) {
            if (request.url.ends_with("/fail")) {
                return {.error = core::TransferError::Network,
                        .error_message = "fake: connection refused"};
            }
            if (request.url.ends_with("/404")) {
                return {.error = core::TransferError::HttpStatus, .http_status_code = 404,
                        .error_message = "fake: server responded with HTTP 404"};
            }
            const std::string body = content_for(request.url);
            std::ofstream(request.part_path, std::ios::binary) << body;
            return {.error = core::TransferError::None, .http_status_code = 200,
                    .bytes_transferred = body.size()};
        }

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool blocked_ = false;
        std::size_t active_ = 0;
        std::size_t max_active_ = 0;
        std::vector<core::TransferRequest> requests_;
    };
}
