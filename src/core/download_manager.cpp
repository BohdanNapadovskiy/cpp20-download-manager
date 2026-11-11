#include "core/download_manager.hpp"

namespace dm::core {
    DownloadManager::DownloadManager(Executor executor, std::size_t thread_count) {

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

    }
}
