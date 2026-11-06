#include "core/http_transfer.hpp"

#include <memory>
#include <stdexcept>
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
}
