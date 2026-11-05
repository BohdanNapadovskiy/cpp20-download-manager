#include "core/curl_global.hpp"
#include <curl/curl.h>

namespace dm::core {
    const char* get_curl_version() {
        return curl_version();
    }
}

