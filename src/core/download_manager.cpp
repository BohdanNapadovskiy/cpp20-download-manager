#include "core/download_manager.hpp"

namespace dm::core {
    DownloadManager::DownloadManager(Executor executor, std::size_t thread_count) : executor_(std::move(executor)) {

        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this](std::stop_token stop_token) {
                worker_loop(std::move(stop_token));
            });
        }
    }

    TaskId DownloadManager::add(std::string url, std::filesystem::path target_path) {
        return 0;
    }

    bool DownloadManager::pause(TaskId id) {
        return false;
    }

    bool DownloadManager::resume(TaskId id) {
        return false;
    }

    bool DownloadManager::cancel(TaskId id) {
        return false;
    }

    std::vector<TaskInfo> DownloadManager::snapshot() const {
        return {};
    }

    void DownloadManager::worker_loop(std::stop_token stop_token) {
        while (true) {
            std::unique_lock lock(mutex_);
            bool has_data = cv_.wait(lock, stop_token, [&]() {
                return !queue_.empty();
            });
            if (!has_data) {
                return;
            }
        }
    }
}
