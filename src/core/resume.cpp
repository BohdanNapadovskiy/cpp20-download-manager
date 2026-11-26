#include "core/resume.hpp"

namespace dm::core {
    std::string if_range_validator(std::string_view etag, std::string_view last_modified) {
        if (etag.starts_with("\"")) {
            return std::string(etag);
        }
        if (!last_modified.empty()) {
            return std::string(last_modified);
        }
        return {};
    }
}
