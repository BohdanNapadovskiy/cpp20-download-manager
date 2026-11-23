#include "core/state_store.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>

namespace dm::core {
    namespace {
        constexpr std::string_view key_id = "id";
        constexpr std::string_view key_target_path = "target_path";
        constexpr std::string_view key_url = "url";
        constexpr std::string_view key_state = "state";
        constexpr std::string_view key_total_bytes = "total_bytes";
        constexpr std::string_view key_error_message = "error_message";
        constexpr std::string_view key_http_status_code = "http_status_code";
        constexpr std::string_view key_tasks = "tasks";
        constexpr std::string_view key_version = "version";

        std::string_view state_to_string(TaskState state) noexcept {
            switch (state) {
                case TaskState::Queued:
                    return "Queued";
                case TaskState::Downloading:
                    return "Downloading";
                case TaskState::Paused:
                    return "Paused";
                case TaskState::Canceled:
                    return "Canceled";
                case TaskState::Failed:
                    return "Failed";
                case TaskState::Retrying:
                    return "Retrying";
                case TaskState::Completed:
                    return "Completed";
            }
            return "Unknown state";
        }

        std::optional<TaskState> string_to_state(std::string_view str_state) noexcept {
            if (str_state == "Queued") {
                return TaskState::Queued;
            }
            if (str_state == "Downloading") {
                return TaskState::Downloading;
            }
            if (str_state == "Paused") {
                return TaskState::Paused;
            }
            if (str_state == "Canceled") {
                return TaskState::Canceled;
            }
            if (str_state == "Failed") {
                return TaskState::Failed;
            }
            if (str_state == "Retrying") {
                return TaskState::Retrying;
            }
            if (str_state == "Completed") {
                return TaskState::Completed;
            }
            return std::nullopt;
        }

        std::string path_to_string(const std::filesystem::path &path) {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
        }

        std::filesystem::path string_to_path(const std::string &path) {
            std::u8string str_utf8(
                reinterpret_cast<const char8_t *>(path.data()),
                path.size()
            );
            return {str_utf8};
        }
    }

    SaveResult save_state(const std::filesystem::path &path, const std::vector<TaskInfo> &tasks) noexcept {
        try {
            auto arr = nlohmann::json::array();
            for (const auto &task: tasks) {
                if (task.state == TaskState::Canceled) {
                    continue;
                }
                nlohmann::json item = {
                    {key_id, task.id},
                    {key_url, task.url},
                    {key_target_path, path_to_string(task.target_path)},
                    {key_state, state_to_string(task.state)},
                    {key_total_bytes, task.total_bytes},
                    {key_error_message, task.error_message},
                    {key_http_status_code, task.http_status_code},
                };
                arr.push_back(std::move(item));
            }
            nlohmann::json root = {
                {key_version, 1},
                {key_tasks, std::move(arr)}
            };
            std::filesystem::path tmp_path = path;
            tmp_path += u8".tmp";
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            {
                std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return {
                        .success = false,
                        .error_message = "save_state() failed: cannot open temporary file: " + path_to_string(tmp_path)
                    };
                }
                out << root.dump(4);
                out.close();
                if (!out) {
                    return {
                        .success = false, .error_message = "save_state() failed: error while writing to temporary file: " + path_to_string(tmp_path)
                    };
                }
            }
            std::filesystem::rename(tmp_path, path);
            return {.success = true, .error_message = {}};
        } catch (const std::exception &e) {
            return {.success = false, .error_message = "save_state() failed: " + std::string(e.what())};
        } catch (...) {
            return {.success = false, .error_message = "save_state() failed: unknown error"};
        }
    }

    LoadResult load_state(const std::filesystem::path &path) noexcept {
        try {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                if (ec) {
                    return {.success = false, .tasks = {}, .error_message = "load_state() failed: " + ec.message()};
                }
                return {.success = true, .tasks = {}, .error_message = {}};
            }
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                return {.success = false, .tasks = {}, .error_message = "load_state() failed: cannot open file"};
            }
            std::string content(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>()
            );
            if (in.bad()) {
                return {
                    .success = false, .tasks = {}, .error_message = "load_state() failed: error while reading file"
                };
            }
            const auto root = nlohmann::json::parse(content);
            const auto &arr = root.at(key_tasks);
            if (!arr.is_array()) {
                return {.success = false, .tasks = {}, .error_message = "load_state() failed: 'tasks' is not an array"};
            }

            std::vector<TaskInfo> loaded_tasks;
            loaded_tasks.reserve(arr.size());

            for (const auto &item: arr) {
                TaskInfo task;
                task.id = item.at(key_id).get<TaskId>();
                task.url = item.at(key_url).get<std::string>();
                task.target_path = string_to_path(item.at(key_target_path).get<std::string>());
                task.total_bytes = item.at(key_total_bytes).get<std::uint64_t>();
                task.error_message = item.at(key_error_message).get<std::string>();
                task.http_status_code = item.at(key_http_status_code).get<int>();
                const auto state_str = item.at(key_state).get<std::string>();
                const auto parsed_state = string_to_state(state_str);
                if (!parsed_state.has_value()) {
                    return {
                        .success = false, .tasks = {},
                        .error_message = "load_state() failed: unknown task state '" + state_str + "'"
                    };
                }

                TaskState st = *parsed_state;
                if (st == TaskState::Downloading || st == TaskState::Retrying) {
                    st = TaskState::Queued;
                }
                task.state = st;
                loaded_tasks.push_back(std::move(task));
            }
            return {.success = true, .tasks = std::move(loaded_tasks), .error_message = ""};
        } catch (const std::exception &e) {
            return {.success = false, .tasks = {}, .error_message = "load_state() failed: " + std::string(e.what())};
        } catch (...) {
            return {.success = false, .tasks = {}, .error_message = "load_state() failed: unknown error"};
        }
    }
}
