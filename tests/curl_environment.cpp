#include "core/curl_global.hpp"

#include <gtest/gtest.h>

#include <optional>

// libcurl must be initialised once before any easy handle is created, exactly as main() does
// with CurlGlobal. A GoogleTest global environment gives the same lifetime for the whole test run.
namespace {
    class CurlEnvironment : public ::testing::Environment {
    public:
        void SetUp() override { curl_.emplace(); }
        void TearDown() override { curl_.reset(); }

    private:
        std::optional<dm::core::CurlGlobal> curl_;
    };

    // GoogleTest takes ownership of the environment.
    [[maybe_unused]] ::testing::Environment* const curl_environment =
            ::testing::AddGlobalTestEnvironment(new CurlEnvironment);
}
