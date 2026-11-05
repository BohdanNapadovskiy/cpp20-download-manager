#include "core/curl_global.hpp"

#include <exception>
#include <iostream>
#include <exception>

int main() {
    try {
        dm::core::CurlGlobal curl_global;
        auto ver = dm::core::get_curl_version();
        std::cout << "Curl version: " << ver << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
