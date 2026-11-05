#include "test_sandbox.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Checks the test infrastructure itself: every later test (.part files, queue state JSON)
// relies on getting a fresh empty directory and on nothing being left behind in %TEMP%.
namespace {
    class SandboxFixtureTest : public dm::test::SandboxTest {};

    TEST_F(SandboxFixtureTest, StartsAsEmptyDirectoryInTemp) {
        ASSERT_TRUE(fs::is_directory(sandbox()));
        EXPECT_TRUE(fs::is_empty(sandbox()));
        EXPECT_TRUE(fs::equivalent(sandbox().parent_path(), fs::temp_directory_path()));
    }

    TEST_F(SandboxFixtureTest, TearDownRemovesNestedContent) {
        const fs::path dir = sandbox();
        fs::create_directories(dir / "downloads");
        std::ofstream(dir / "downloads" / "file.bin.part", std::ios::binary) << "partial";
        std::ofstream(dir / "state.json") << "{}";

        TearDown();
        EXPECT_FALSE(fs::exists(dir));

        SetUp();  // give the framework's own TearDown a directory to clean
    }

    TEST_F(SandboxFixtureTest, EachSetUpCreatesDistinctDirectory) {
        const fs::path first = sandbox();
        TearDown();
        SetUp();
        EXPECT_NE(sandbox(), first);
    }
}
