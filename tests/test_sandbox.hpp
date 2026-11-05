#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace dm::test {
    // Base fixture: every test gets its own empty directory under %TEMP%, removed in TearDown.
    // Unlike 01-file-manager, downloads never create junctions or symlinks, so
    // std::filesystem::remove_all is safe here and no WinAPI walk is needed.
    class SandboxTest : public ::testing::Test {
    protected:
        void SetUp() override {
            static std::atomic<unsigned> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            sandbox_ = std::filesystem::temp_directory_path() /
                       ("dm-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            std::filesystem::create_directories(sandbox_);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(sandbox_, ec);
        }

        const std::filesystem::path& sandbox() const { return sandbox_; }

    private:
        std::filesystem::path sandbox_;
    };
}
