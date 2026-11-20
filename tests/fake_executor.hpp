#pragma once

#include "core/http_transfer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
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
    //   * a call reports progress through request.on_progress like download_once: a held call
    //     reports (partial size, full size), a successful call reports (full size, full size),
    //     where full size is content_for(url).size();
    //   * report_held(n) sends one more progress report from every held call;
    //   * wait_started(n) waits until n calls have begun;
    //   * the outcome depends on the URL: ".../fail" -> Network error, ".../404" -> HttpStatus,
    //     anything else -> 200 and the file content "content of <url>" written to part_path;
    //   * fail_next(url, result) makes the next call for that URL return `result` instead, once;
    //     several fail_next calls are used up in order, then the URL behaves as usual again.
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

        // The next call for `url` returns `result` without downloading anything. Queued per URL:
        // fail_next(u, a); fail_next(u, b) -> the first call gets a, the second b, the third
        // downloads as usual.
        void fail_next(const std::string& url, core::TransferResult result) {
            std::scoped_lock lock(mutex_);
            scripted_[url].push_back(std::move(result));
        }

        // Sends one more progress report (downloaded, full size) from every call held in block(),
        // as if more data had arrived. Call it only while the calls are held: the requests
        // belong to the running calls.
        void report_held(std::size_t downloaded) {
            std::vector<const core::TransferRequest*> held;
            {
                std::scoped_lock lock(mutex_);
                held = held_;
            }
            for (const core::TransferRequest* request : held) {
                report_progress(*request, downloaded, content_for(request->url).size());
            }
        }

        // When false, a held call reports no progress: lets a test look at the progress a task has
        // right after a download starts, before the executor reports anything.
        void set_report_partial(bool report) {
            std::scoped_lock lock(mutex_);
            report_partial_ = report;
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

        // How many calls were made for `url`.
        std::size_t calls_for(const std::string& url) const {
            std::scoped_lock lock(mutex_);
            return static_cast<std::size_t>(std::ranges::count(requests_, url, &core::TransferRequest::url));
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
                const bool report = report_partial_;
                lock.unlock();
                std::ofstream(request.part_path, std::ios::binary) << partial_content();
                if (report) {
                    report_progress(request, partial_content().size(), content_for(request.url).size());
                }
                lock.lock();
                held_.push_back(&request);
                // false: the stop token was stopped before release() -> interrupted download
                const bool released = cv_.wait(lock, request.stop_token, [&] { return !blocked_; });
                std::erase(held_, &request);
                if (!released) {
                    --active_;
                    ++aborted_;
                    return {.error = core::TransferError::Aborted,
                            .error_message = "fake: stop requested"};
                }
            }

            if (auto scripted = scripted_.find(request.url);
                scripted != scripted_.end() && !scripted->second.empty()) {
                core::TransferResult result = std::move(scripted->second.front());
                scripted->second.pop_front();
                --active_;
                return result;
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
            report_progress(request, body.size(), body.size());
            return {.error = core::TransferError::None, .http_status_code = 200,
                    .bytes_transferred = body.size()};
        }

        // Called without the fake's own lock held, like download_once calls on_progress.
        static void report_progress(const core::TransferRequest& request, std::size_t downloaded,
                                    std::size_t total) {
            if (request.on_progress) {
                request.on_progress(downloaded, total);
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable_any cv_;   // _any: it also wakes on request.stop_token
        bool blocked_ = false;
        bool report_partial_ = true;
        std::vector<const core::TransferRequest*> held_;   // calls waiting in block()
        std::size_t active_ = 0;
        std::size_t max_active_ = 0;
        std::size_t aborted_ = 0;
        std::vector<core::TransferRequest> requests_;
        std::map<std::string, std::deque<core::TransferResult>> scripted_;   // fail_next()
    };
}
