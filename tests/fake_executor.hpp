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
    //   * block()/release() hold every call in the middle of the "download" until the test lets
    //     it go; a held call has already written partial_content() to part_path, like a real
    //     download that received part of the body;
    //   * a call honours request.stop_token like download_once: a token stopped before the call
    //     aborts at once; a held call that is stopped returns TransferError::Aborted and leaves
    //     the partial .part file;
    //   * wait_started(n) waits until n calls have begun;
    //   * the outcome depends on the URL: ".../fail" -> Network error, ".../404" -> HttpStatus,
    //     anything else -> 200 and the file content "content of <url>" written to part_path.
    // Records peak concurrency, every request, the order in which calls started and the number
    // of calls that ended as Aborted.
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

        std::size_t aborted() const {
            std::scoped_lock lock(mutex_);
            return aborted_;
        }

        static std::string content_for(const std::string& url) { return "content of " + url; }
        static std::string partial_content() { return "partial"; }

    private:
        core::TransferResult run(const core::TransferRequest& request) {
            std::unique_lock lock(mutex_);
            requests_.push_back(request);
            ++active_;
            max_active_ = std::max(max_active_, active_);
            cv_.notify_all();

            // Like download_once: a token that is already stopped aborts at once. Checked under
            // the same lock that records the request, so a test sees it right after wait_started.
            if (request.stop_token.stop_requested()) {
                --active_;
                ++aborted_;
                return {.error = core::TransferError::Aborted,
                        .error_message = "fake: stop requested"};
            }

            if (blocked_) {
                lock.unlock();
                std::ofstream(request.part_path, std::ios::binary) << partial_content();
                lock.lock();
                // false: the stop token was stopped before release() -> interrupted download
                if (!cv_.wait(lock, request.stop_token, [&] { return !blocked_; })) {
                    --active_;
                    ++aborted_;
                    return {.error = core::TransferError::Aborted,
                            .error_message = "fake: stop requested"};
                }
            }
            lock.unlock();

            core::TransferResult result = outcome(request);

            lock.lock();
            --active_;
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
        std::condition_variable_any cv_;   // _any: it also wakes on request.stop_token
        bool blocked_ = false;
        std::size_t active_ = 0;
        std::size_t max_active_ = 0;
        std::size_t aborted_ = 0;
        std::vector<core::TransferRequest> requests_;
    };
}
