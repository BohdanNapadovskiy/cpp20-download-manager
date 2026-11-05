#include "core/curl_global.hpp"

#include <string>
#include <stdexcept>
#include <curl/curl.h>

namespace dm::core {
    CurlGlobal::CurlGlobal() {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error("curl_global_init() failed: " + std::string(curl_easy_strerror(result)));
        }
    }

    CurlGlobal::~CurlGlobal() {
        curl_global_cleanup();
    }

    const char *get_curl_version() {
        return curl_version();
    }
}
