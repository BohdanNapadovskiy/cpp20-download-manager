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

    DownloadManager::~DownloadManager() {
        for (auto &worker: workers_) {
            worker.request_stop();
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
        if (it->second.info.state == TaskState::Queued) {
            it->second.info.state = TaskState::Paused;
            std::erase(queue_, id);
            return true;
        } else if (it->second.info.state == TaskState::Downloading) {
            it->second.stop_reason = StopReason::Paused;
            it->second.stop.request_stop();
            return true;
        }
        return false;
    }

    bool DownloadManager::resume(TaskId id) {
        {
            std::lock_guard lock(mutex_);
            auto it = tasks_.find(id);
            if (it == tasks_.end() || it->second.info.state != TaskState::Paused) {
                return false;
            }
            it->second.stop = std::stop_source{};
            it->second.stop_reason = StopReason::None;
            it->second.info.state = TaskState::Queued;
            queue_.emplace_back(id);
        }
        cv_.notify_one();
        return true;
    }

    bool DownloadManager::cancel(TaskId id) {
        std::filesystem::path target_path;
        bool result = false;
        {
            std::lock_guard lock(mutex_);
            auto it = tasks_.find(id);
            if (it == tasks_.end()) {
                return result;
            }
            if (it->second.info.state == TaskState::Queued) {
                it->second.info.state = TaskState::Canceled;
                std::erase(queue_, id);
                target_path = it->second.info.target_path;
                result = true;
            } else if (it->second.info.state == TaskState::Paused) {
                it->second.info.state = TaskState::Canceled;
                target_path = it->second.info.target_path;
                result = true;
            } else if (it->second.info.state == TaskState::Downloading) {
                it->second.stop_reason = StopReason::Canceled;
                it->second.stop.request_stop();
                result = true;
            }
        }
        if (!target_path.empty()) {
            std::error_code ec;
            std::filesystem::path part_path = target_path;
            part_path += ".part";
            std::filesystem::remove(part_path, ec);
        }
        return result;
    }

    std::vector<TaskInfo> DownloadManager::snapshot() const {
        std::lock_guard lock(mutex_);
        std::vector<TaskInfo> result;
        result.reserve(tasks_.size());
        for (const auto &[id, task]: tasks_) {
            result.push_back(task.info);
        }
        return result;
    }

    void DownloadManager::worker_loop(std::stop_token stop_token) {
        while (true) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop_token, [&]() {
                return !queue_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            std::string url;
            std::filesystem::path target_path;
            std::filesystem::path part_path;
            auto current_id = queue_.front();
            std::stop_source task_stop;
            queue_.pop_front();
            auto it = tasks_.find(current_id);
            if (it != tasks_.end()) {
                task_stop = it->second.stop;
                it->second.info.state = TaskState::Downloading;
                it->second.info.downloaded_bytes = 0;
                it->second.info.total_bytes = 0;
                url = it->second.info.url;
                target_path = it->second.info.target_path;
                part_path = target_path;
                part_path += ".part";
            } else {
                continue;
            }
            lock.unlock();
            auto request = TransferRequest{
                .url = std::move(url),
                .part_path = part_path,
                .stop_token = task_stop.get_token(),
                .on_progress = [this, current_id] (std::uint64_t downloaded, std::uint64_t total) {
                    std::lock_guard progress_lock(mutex_);
                    auto task_it = tasks_.find(current_id);
                    if (task_it != tasks_.end()) {
                        task_it->second.info.downloaded_bytes = downloaded;
                        task_it->second.info.total_bytes = total;
                    }
                }
            };
            TransferResult result;
            {
                std::stop_callback link_to_worker(stop_token, [&task_stop] {
                    task_stop.request_stop();
                });
                result = executor_(request);
            }
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
                task_it->second.info.http_status_code = result.http_status_code;
                task_it->second.info.error_message = result.error_message;
                if (result.ok() && !ec) {
                    task_it->second.info.state = TaskState::Completed;
                } else if (result.error == TransferError::Aborted) {
                    if (task_it->second.stop_reason == StopReason::Paused) {
                        task_it->second.info.state = TaskState::Paused;
                    } else if (task_it->second.stop_reason == StopReason::Canceled) {
                        task_it->second.info.state = TaskState::Canceled;
                        std::filesystem::remove(part_path, ec);
                    } else {
                        task_it->second.info.state = TaskState::Queued;
                    }
                } else {
                    task_it->second.info.state = TaskState::Failed;
                }
            }
        }
    }
}
