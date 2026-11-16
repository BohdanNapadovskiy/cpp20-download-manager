#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace dm::core {
    enum class TransferError {
        None,
        Network,
        Tls,
        DiskWrite,
        HttpStatus,
        Aborted
    };

    using TaskId = std::uint64_t;

    enum class TaskState {
        Queued,
        Downloading,
        Paused,
        Canceled,
        Failed,
        Completed
    };

    struct TaskInfo {
        TaskId id = 0;
        std::string url{};
        TaskState state = TaskState::Queued;
        std::filesystem::path target_path{};
        int http_status_code = 0;
        std::string error_message{};
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t total_bytes = 0;
    };
}
