#include <iostream>
#include "core/curl_global.hpp"

int main() {
    auto ver = dm::core::get_curl_version();
    std::cout << "Curl version: " << ver << "\n";
    return 0;
}