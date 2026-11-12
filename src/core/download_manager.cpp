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
        TaskId id{};
        {
            std::lock_guard lock(mutex_);
            id = next_id_++;
            tasks_.emplace(id, TaskInfo{
                               .id = id,
                               .url = std::move(url),
                               .state = TaskState::Queued,
                               .target_path = std::move(target_path),
                           });
            queue_.emplace_back(id);
        }
        cv_.notify_one();
        return id;
    }

    bool DownloadManager::pause(TaskId id) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            return false;
        }
        if (it->second.state == TaskState::Queued) {
            it->second.state = TaskState::Paused;
            std::erase(queue_, id);
            return true;
        }
        return false;
    }

    bool DownloadManager::resume(TaskId id) {
        {
            std::lock_guard lock(mutex_);
            auto it = tasks_.find(id);
            if (it == tasks_.end() || it->second.state != TaskState::Paused) {
                return false;
            }
            it->second.state = TaskState::Queued;
            queue_.emplace_back(id);
        }
        cv_.notify_one();
        return true;
    }

    bool DownloadManager::cancel(TaskId id) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            return false;
        }
        if (it->second.state == TaskState::Queued || it->second.state == TaskState::Paused) {
            it->second.state = TaskState::Canceled;
            std::erase(queue_, id);
            return true;
        }
        return false;
    }

    std::vector<TaskInfo> DownloadManager::snapshot() const {
        std::lock_guard lock(mutex_);

        std::vector<TaskInfo> result;
        result.reserve(tasks_.size());

        for (const auto &[id, info]: tasks_) {
            result.push_back(info);
        }

        return result;
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
            std::string url;
            std::filesystem::path target_path;
            std::filesystem::path part_path;
            auto current_id = queue_.front();
            queue_.pop_front();
            auto it = tasks_.find(current_id);
            if (it != tasks_.end()) {
                it->second.state = TaskState::Downloading;
                url = it->second.url;
                target_path = it->second.target_path;
                part_path = target_path;
                part_path += ".part";
            } else {
                continue;
            }
            lock.unlock();
            auto request = TransferRequest{.url = std::move(url), .part_path = part_path};
            TransferResult result = executor_(request);
            std::error_code ec;
            if (result.ok()) {
                std::filesystem::rename(part_path, target_path, ec);
                if (ec) {
                    result.error_message = "rename() failed: " + ec.message();
                }
            }
            lock.lock();
            auto task_it = tasks_.find(current_id);
            if (task_it != tasks_.end()) {
                task_it->second.http_status_code = result.http_status_code;
                task_it->second.error_message = result.error_message;
                if (result.ok() && !ec) {
                    task_it->second.state = TaskState::Completed;
                } else {
                    task_it->second.state = TaskState::Failed;
                }
            }
        }
    }
}
