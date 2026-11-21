#include "core/download_manager.hpp"
#include <optional>

namespace dm::core {
    DownloadManager::DownloadManager(Executor executor, std::size_t thread_count,
                                     RetryPolicy retry_policy) : executor_(std::move(executor)),
                                                                 retry_policy_(retry_policy) {
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
        } else if (it->second.info.state == TaskState::Downloading || it->second.info.state == TaskState::Retrying) {
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
            if (it == tasks_.end()) {
                return false;
            }
            const bool resumable = it->second.info.state == TaskState::Paused
                                   || it->second.info.state == TaskState::Failed;
            if (!resumable) {
                return false;
            }
            it->second.stop = std::stop_source{};
            it->second.info.attempt = 0;
            it->second.info.error_message.clear();
            it->second.info.http_status_code = 0;
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
            } else if (it->second.info.state == TaskState::Downloading || it->second.info.state ==
                       TaskState::Retrying) {
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

    std::optional<DownloadManager::Job> DownloadManager::take_next(std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, stop_token, [&]() {
                return !queue_.empty();
            });
            if (stop_token.stop_requested()) {
                return std::nullopt;
            }
            auto current_id = queue_.front();
            queue_.pop_front();
            auto it = tasks_.find(current_id);
            if (it == tasks_.end()) {
                continue;
            }
            it->second.info.state = TaskState::Downloading;
            std::filesystem::path part_path = it->second.info.target_path;
            part_path += ".part";
            return Job{
                .id = current_id,
                .url = it->second.info.url,
                .target_path = it->second.info.target_path,
                .part_path = part_path,
                .task_stop = it->second.stop
            };
        }
    }

    TransferResult DownloadManager::download_with_retries(Job &job, std::stop_token stop_token) {
        auto current_id = job.id;
        auto request = TransferRequest{
            .url = job.url,
            .part_path = job.part_path,
            .stop_token = job.task_stop.get_token(),
            .on_progress = [this, current_id](std::uint64_t downloaded, std::uint64_t total) {
                std::lock_guard progress_lock(mutex_);
                auto task_it = tasks_.find(current_id);
                if (task_it != tasks_.end()) {
                    task_it->second.info.downloaded_bytes = downloaded;
                    task_it->second.info.total_bytes = total;
                    task_it->second.info.speed_bps = task_it->second.speed_meter.update(
                        downloaded, SpeedMeter::Clock::now());
                }
            }
        };
        TransferResult result;
        {
            std::stop_callback link_to_worker(stop_token, [&job] {
                job.task_stop.request_stop();
            });
            std::uint32_t attempt = 1;
            while (true) {
                begin_attempt(job.id, attempt);
                result = executor_(request);
                if (!should_retry(result) || result.ok() || attempt >= retry_policy_.max_attempts) {
                    break;
                }
                if (!wait_before_retry(job.id, result, attempt + 1, job.task_stop.get_token())) {
                    result.error = TransferError::Aborted;
                    result.error_message = "wait_before_retry() aborted: stop requested";
                    result.http_status_code = 0;
                    break;
                }
                attempt++;
            }
        }
        return result;
    }

    void DownloadManager::begin_attempt(TaskId task_id, std::uint32_t attempt) {
        std::lock_guard lock(mutex_);
        auto task_it = tasks_.find(task_id);
        if (task_it != tasks_.end()) {
            task_it->second.info.attempt = attempt;
            task_it->second.info.state = TaskState::Downloading;
            task_it->second.info.downloaded_bytes = 0;
            task_it->second.info.total_bytes = 0;
            task_it->second.info.speed_bps = 0;
            task_it->second.speed_meter.start(SpeedMeter::Clock::now());
        }
    }

    bool DownloadManager::wait_before_retry(TaskId task_id, const TransferResult &result, std::uint32_t next_attempt,
                                            std::stop_token stop_token) {
        std::unique_lock lock(mutex_);
        auto task_it = tasks_.find(task_id);
        if (task_it == tasks_.end()) {
            return false;
        }
        task_it->second.info.state = TaskState::Retrying;
        task_it->second.info.error_message = result.error_message;
        task_it->second.info.http_status_code = result.http_status_code;
        task_it->second.info.speed_bps = 0;
        auto delay = retry_delay(retry_policy_, next_attempt);
        std::condition_variable_any retry_wait;
        retry_wait.wait_for(lock, stop_token, delay, []() {
            return false;
        });
        return !stop_token.stop_requested();
    }

    void DownloadManager::finish(const Job &job, TransferResult &transfer_result) {
        std::error_code ec;
        if (transfer_result.ok()) {
            std::filesystem::rename(job.part_path, job.target_path, ec);
            if (ec) {
                transfer_result.error_message = "rename() failed: " + ec.message();
            }
        }
        bool need_remove = false;
        {
            std::lock_guard lock(mutex_);
            auto task_it = tasks_.find(job.id);
            if (task_it != tasks_.end()) {
                task_it->second.info.http_status_code = transfer_result.http_status_code;
                task_it->second.info.error_message = transfer_result.error_message;
                if (transfer_result.ok() && !ec) {
                    task_it->second.info.state = TaskState::Completed;
                } else if (transfer_result.error == TransferError::Aborted) {
                    if (task_it->second.stop_reason == StopReason::Paused) {
                        task_it->second.info.state = TaskState::Paused;
                    } else if (task_it->second.stop_reason == StopReason::Canceled) {
                        task_it->second.info.state = TaskState::Canceled;
                        need_remove = true;
                    } else {
                        task_it->second.info.state = TaskState::Queued;
                    }
                } else {
                    task_it->second.info.state = TaskState::Failed;
                }
                task_it->second.info.speed_bps = 0;
            }
        }
        if (need_remove) {
            std::error_code error_code;
            std::filesystem::remove(job.part_path, error_code);
        }
    }

    void DownloadManager::worker_loop(std::stop_token stop_token) {
        while (auto task = take_next(stop_token)) {
            auto result = download_with_retries(*task, stop_token);
            finish(*task, result);
        }
    }
}
