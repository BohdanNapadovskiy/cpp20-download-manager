#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using dm::core::download_once;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 3.2 of docs/IMPLEMENTATION_PLAN.md):
//   * request.on_progress is called during the download with (downloaded, total);
//   * downloaded never goes down and the last report equals the full size of the body;
//   * total is the size announced by Content-Length; it is 0 while the size is unknown
//     (a chunked response has no Content-Length, so it stays 0 to the end);
//   * an exception thrown by on_progress does not stop or break the download.
namespace {
    constexpr std::size_t file_size = 1024 * 1024;
    constexpr std::size_t chunk_size = 64 * 1024;

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    using Report = std::pair<std::uint64_t, std::uint64_t>;   // (downloaded, total)

    // Routes:
    //   /file     200, 1 MiB with Content-Length
    //   /chunked  200, 1 MiB in chunked encoding: no Content-Length, size unknown in advance
    class DownloadOnceProgressTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            auto& server = server_.server();
            server.Get("/file", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/chunked", [this](const httplib::Request&, httplib::Response& res) {
                res.set_chunked_content_provider(
                        "application/octet-stream",
                        [this](std::size_t offset, httplib::DataSink& sink) {
                            if (offset >= content_.size()) {
                                sink.done();
                                return true;
                            }
                            return sink.write(content_.data() + offset, chunk_size);
                        });
            });
            server_.start();
        }

        void TearDown() override {
            server_.stop();
            SandboxTest::TearDown();
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        // Downloads `path`, recording every progress report. download_once calls on_progress
        // on its own thread, synchronously, so no locking is needed.
        TransferResult download(const std::string& path) {
            TransferRequest request{.url = server_.url(path), .part_path = part_path()};
            request.on_progress = [this](std::uint64_t downloaded, std::uint64_t total) {
                reports_.emplace_back(downloaded, total);
            };
            return download_once(request);
        }

        const std::string content_ = dm::test::make_content(file_size);
        std::vector<Report> reports_;
        dm::test::TestServer server_;
    };

    TEST_F(DownloadOnceProgressTest, ProgressIsReportedDuringDownload) {
        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << "error message: " << result.error_message;
        EXPECT_FALSE(reports_.empty()) << "on_progress was never called";
    }

    TEST_F(DownloadOnceProgressTest, DownloadedNeverDecreases) {
        ASSERT_TRUE(download("/file").ok());
        ASSERT_FALSE(reports_.empty());

        for (std::size_t i = 1; i < reports_.size(); ++i) {
            EXPECT_GE(reports_[i].first, reports_[i - 1].first) << "report " << i << " went back";
        }
    }

    TEST_F(DownloadOnceProgressTest, LastReportEqualsFileSize) {
        ASSERT_TRUE(download("/file").ok());
        ASSERT_FALSE(reports_.empty());

        EXPECT_EQ(reports_.back().first, file_size) << "the final report must show 100%";
    }

    TEST_F(DownloadOnceProgressTest, TotalIsContentLengthOnceKnown) {
        ASSERT_TRUE(download("/file").ok());

        bool total_seen = false;
        for (const auto& [downloaded, total] : reports_) {
            if (total != 0) {
                total_seen = true;
                EXPECT_EQ(total, file_size);
            }
            EXPECT_TRUE(total == 0 || downloaded <= total) << downloaded << " of " << total;
        }
        EXPECT_TRUE(total_seen) << "Content-Length was sent, so total must become known";
    }

    TEST_F(DownloadOnceProgressTest, UnknownSizeIsReportedAsZeroTotal) {
        const TransferResult result = download("/chunked");
        ASSERT_TRUE(result.ok()) << "error message: " << result.error_message;
        ASSERT_FALSE(reports_.empty());

        for (const auto& [downloaded, total] : reports_) {
            EXPECT_EQ(total, 0u) << "no Content-Length: the size is unknown";
        }
        EXPECT_EQ(reports_.back().first, file_size);
        EXPECT_TRUE(read_file(part_path()) == content_);
    }

    TEST_F(DownloadOnceProgressTest, ThrowingCallbackDoesNotStopDownload) {
        TransferRequest request{.url = server_.url("/file"), .part_path = part_path()};
        request.on_progress = [](std::uint64_t, std::uint64_t) {
            throw std::runtime_error("progress observer failed");
        };

        const TransferResult result = download_once(request);   // must not terminate the program

        EXPECT_TRUE(result.ok()) << "a broken progress observer is not a reason to fail: "
                                 << result.error_message;
        EXPECT_TRUE(read_file(part_path()) == content_);
    }
}
