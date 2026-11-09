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

    // Success means the body was received and written: 200 (full file) or 206 (resumed
    // range). Other 2xx codes are not a download: 204 has no body, so it would leave an
    // empty file that looks complete.
    [[nodiscard]] constexpr bool is_success_status(int status) noexcept {
        return status == 200 || status == 206;
    }


    struct TransferResult {
        TransferError error = TransferError::None;
        int http_status_code = 0;
        std::string error_message{};
        std::uintmax_t bytes_transferred = 0;

        [[nodiscard]] bool ok() const {
            return error == TransferError::None &&
                   is_success_status(http_status_code);
        }
    };

    [[nodiscard]] TransferResult download_once(const TransferRequest &request);
}
