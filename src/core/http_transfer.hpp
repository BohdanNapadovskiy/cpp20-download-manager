#pragma once
#include "core/types.hpp"

#include <filesystem>
#include <string>
#include <chrono>
#include <cstdint>

namespace dm::core {
    struct TransferRequest {
        std::string url;
        std::filesystem::path part_path;
        std::chrono::seconds connect_timeout = std::chrono::seconds(10);
    };

    struct TransferResult {
        TransferError error = TransferError::None;
        int http_status_code = 0;
        std::string error_message{};
        std::uintmax_t bytes_transferred = 0;

        // Success means the body was received and written: 200 (full file) or 206 (resumed
        // range). Other 2xx codes are not a download: 204 has no body, so it would leave an
        // empty file that looks complete.
        [[nodiscard]] bool ok() const {
            return error == TransferError::None && (http_status_code == 200 || http_status_code == 206);
        }
    };

    [[nodiscard]] TransferResult download_once(const TransferRequest &request);
}
