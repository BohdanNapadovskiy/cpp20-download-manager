#pragma once
#include <string>
#include <string_view>

namespace dm::core {
    [[nodiscard]] std::string if_range_validator(std::string_view etag, std::string_view last_modified);
}

