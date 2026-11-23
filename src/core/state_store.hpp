#pragma once

#include "core/types.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace dm::core {
    struct LoadResult {
        bool success = false;
        std::vector<TaskInfo> tasks;
        std::string error_message{};
    };

    struct SaveResult {
        bool success = false;
        std::string error_message{};
    };

    [[nodiscard]] SaveResult save_state(const std::filesystem::path &path, const std::vector<TaskInfo> &tasks) noexcept;
    [[nodiscard]] LoadResult load_state(const std::filesystem::path &path) noexcept;
}
