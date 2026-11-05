#pragma once

namespace dm::core {
    class CurlGlobal {
    public:
        CurlGlobal();
        CurlGlobal(const CurlGlobal &) = delete;
        CurlGlobal &operator=(const CurlGlobal &) = delete;
        ~CurlGlobal();
    };

    const char *get_curl_version();
}
