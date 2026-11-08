#include "core/http_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <curl/curl.h>

namespace dm::core {
    namespace {
        using EasyHandle = std::unique_ptr<CURL, decltype([](CURL *h) {
            curl_easy_cleanup(h);
        })>;

        EasyHandle create_easy_handle() {
            CURL *handle = curl_easy_init();
            if (handle == nullptr) {
                throw std::runtime_error("curl_easy_init() failed: returned nullptr");
            }
            return EasyHandle{handle};
        }
    }

    TransferResult download_once(const TransferRequest &request) {
        EasyHandle handle = create_easy_handle();
        char error_buffer[CURL_ERROR_SIZE];
        error_buffer[0] = '\0';
        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer);
        const CURLcode code = curl_easy_perform(handle.get());
        if (code != CURLE_OK) {
            std::string error_msg;
            if (error_buffer[0] != '\0') {
                error_msg = error_buffer;
            } else {
                error_msg = curl_easy_strerror(code);
            }
            return TransferResult{.error = TransferError::Network, .error_message = error_msg};
        }
        return TransferResult{.error = TransferError::None};
    }
}
