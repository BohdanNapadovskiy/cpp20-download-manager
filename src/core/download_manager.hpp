#pragma once
#include "core/types.hpp"
#include "core/http_transfer.hpp"
#include "core/speed_meter.hpp"
#include "core/retry.hpp"
#include <cstddef>
#include <string>
#include <filesystem>
#include <functional>
#include <vector>
#include <mutex>
#include <stop_token>
#include <condition_variable>
#include <thread>
#include <deque>
#include <unordered_map>
#include <optional>

namespace dm::core {
    class DownloadManager {
    public:
        using Executor = std::function<decltype(download_once)>;
        using StateListener = std::function<void(const std::vector<TaskInfo> &)>;

        explicit DownloadManager(Executor executor,
                                 std::size_t thread_count, RetryPolicy retry_policy = RetryPolicy{},
                                 StateListener state_listener = StateListener{});

        ~DownloadManager();

        // Забороняємо копіювання та переміщення, щоб вказівник `this` у потоках залишався валідним
        DownloadManager(const DownloadManager &) = delete;

        DownloadManager &operator=(const DownloadManager &) = delete;

        DownloadManager(DownloadManager &&) = delete;

        DownloadManager &operator=(DownloadManager &&) = delete;

        [[nodiscard]] TaskId add(std::string url, std::filesystem::path target_path);

        [[nodiscard]] bool pause(TaskId id);

        [[nodiscard]] bool resume(TaskId id);

        [[nodiscard]] bool cancel(TaskId id);

        [[nodiscard]] std::vector<TaskInfo> snapshot() const;

        [[nodiscard]] std::size_t restore(std::vector<TaskInfo> tasks);

    private:
        enum class StopReason {
            Paused,
            Canceled,
            None
        };

        struct StateChange {
            std::uint64_t number = 0;
            std::vector<TaskInfo> tasks{};
        };

        struct Task {
            TaskInfo info;
            std::stop_source stop;
            StopReason stop_reason = StopReason::None;
            SpeedMeter speed_meter;
        };

        struct Job {
            TaskId id = 0;
            std::string url;
            std::filesystem::path target_path;
            std::filesystem::path part_path;
            std::stop_source task_stop;
        };

        void worker_loop(std::stop_token stop_token);

        std::optional<Job> take_next(std::stop_token stop_token);

        TransferResult download_with_retries(Job &job, std::stop_token stop_token);

        void begin_attempt(TaskId task_id, std::uint32_t attempt);

        [[nodiscard]] bool wait_before_retry(TaskId task_id, const TransferResult &result, std::uint32_t next_attempt,
                                             std::stop_token stop_token);

        void finish(const Job &job, TransferResult &transfer_result);

        StateChange record_change_locked();

        std::vector<TaskInfo> snapshot_locked() const;

        void notify(const StateChange& change);

        Executor executor_;
        // mutable дозволяє блокувати м'ютекс усередині const-методу snapshot()
        mutable std::mutex mutex_;
        std::mutex notify_mutex_;
        std::condition_variable_any cv_;
        TaskId next_id_{1};
        std::unordered_map<TaskId, Task> tasks_;
        std::deque<TaskId> queue_;
        RetryPolicy retry_policy_;
        StateListener state_listener_;
        // Потоки ОБОВ'ЯЗКОВО останніми: вони знищаться першими,
        // гарантуючи, що mutex_, cv_, tasks_, state_listener_ та queue_ живі до завершення всіх worker_loop.
        std::uint64_t change_seq_ = 0;
        std::uint64_t last_notified_seq_ = 0;
        std::vector<std::jthread> workers_;

    };
}
