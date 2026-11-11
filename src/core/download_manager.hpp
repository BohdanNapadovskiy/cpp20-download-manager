#pragma once
#include "core/types.hpp"
#include "core/http_transfer.hpp"
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

namespace dm::core {
    class DownloadManager {
    public:
        using Executor = std::function<decltype(download_once)>;
        explicit DownloadManager(Executor executor, std::size_t thread_count);
        ~DownloadManager() = default;
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
    private:
        void worker_loop(std::stop_token stop_token);
        Executor executor_;
        // mutable дозволяє блокувати м'ютекс усередині const-методу snapshot()
        mutable std::mutex mutex_;
        std::condition_variable_any cv_;
        TaskId next_id_{1};
        std::unordered_map<TaskId, TaskInfo> tasks_;
        std::deque<TaskId> queue_;

        // Потоки ОБОВ'ЯЗКОВО останніми: вони знищаться першими,
        // гарантуючи, що mutex_, cv_, tasks_ та queue_ живі до завершення всіх worker_loop.
        std::vector<std::jthread> workers_;
    };
}
