#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

using dm::core::download_once;
using dm::core::TransferError;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 1 of docs/IMPLEMENTATION_PLAN.md):
//   * download_once writes the response body to exactly request.part_path (renaming is the
//     caller's job);
//   * an HTTP error status (404, 500) is reported as TransferError::HttpStatus with the code,
//     and the error page body never reaches the file;
//   * redirects are followed; the result describes the final response;
//   * a refused connection is TransferError::Network with a human-readable message;
//   * a file that cannot be opened for writing is TransferError::DiskWrite.
namespace {
    constexpr std::size_t file_size = 1024 * 1024;  // 1 MiB: many write-callback calls

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // Routes:
    //   /file     200, 1 MiB of binary content
    //   /missing  404 with an HTML error page
    //   /broken   500 with an HTML error page
    //   /old      301 -> /file
    class DownloadOnceTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            auto& server = server_.server();
            server.Get("/file", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/missing", [](const httplib::Request&, httplib::Response& res) {
                res.status = 404;
                res.set_content(error_page, "text/html");
            });
            server.Get("/broken", [](const httplib::Request&, httplib::Response& res) {
                res.status = 500;
                res.set_content(error_page, "text/html");
            });
            server.Get("/old", [](const httplib::Request&, httplib::Response& res) {
                res.set_redirect("/file", 301);
            });
            server_.start();
        }

        void TearDown() override {
            server_.stop();
            SandboxTest::TearDown();
        }

        TransferResult download(const std::string& path) {
            return download_once(TransferRequest{.url = server_.url(path), .part_path = part_path()});
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        // True when the file is absent or empty: no bytes of an error response were written.
        bool part_has_no_data() const {
            return !fs::exists(part_path()) || fs::file_size(part_path()) == 0;
        }

        static constexpr const char* error_page = "<html><body>error page</body></html>";
        const std::string content_ = dm::test::make_content(file_size);
        dm::test::TestServer server_;
    };

    // --- 200 -------------------------------------------------------------------------------

    TEST_F(DownloadOnceTest, Status200IsSuccess) {
        const TransferResult result = download("/file");

        EXPECT_TRUE(result.ok()) << "error message: " << result.error_message;
        EXPECT_EQ(result.error, TransferError::None);
        EXPECT_EQ(result.http_status_code, 200);
    }

    TEST_F(DownloadOnceTest, Status200WritesExactBodyToPartPath) {
        const TransferResult result = download("/file");

        ASSERT_TRUE(fs::exists(part_path()));
        const std::string written = read_file(part_path());
        ASSERT_EQ(written.size(), content_.size());
        EXPECT_TRUE(written == content_) << "file content differs from what the server sent";
    }

    TEST_F(DownloadOnceTest, Status200ReportsBytesTransferred) {
        const TransferResult result = download("/file");

        EXPECT_EQ(result.bytes_transferred, file_size);
    }

    TEST_F(DownloadOnceTest, Status200DoesNotRenamePartFile) {
        const TransferResult result = download("/file");

        EXPECT_TRUE(fs::exists(part_path()));
        EXPECT_FALSE(fs::exists(sandbox() / "file.bin")) << "renaming is the caller's job";
    }

    // --- HTTP errors -----------------------------------------------------------------------

    TEST_F(DownloadOnceTest, Status404IsHttpStatusError) {
        const TransferResult result = download("/missing");

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::HttpStatus);
        EXPECT_EQ(result.http_status_code, 404);
    }

    TEST_F(DownloadOnceTest, Status404DoesNotWriteErrorPage) {
        const TransferResult result = download("/missing");

        EXPECT_TRUE(part_has_no_data()) << "the 404 page must not end up in the .part file";
    }

    TEST_F(DownloadOnceTest, Status500IsHttpStatusErrorWithoutBody) {
        const TransferResult result = download("/broken");

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::HttpStatus);
        EXPECT_EQ(result.http_status_code, 500);
        EXPECT_TRUE(part_has_no_data()) << "the 500 page must not end up in the .part file";
    }

    // --- Redirect --------------------------------------------------------------------------

    TEST_F(DownloadOnceTest, Redirect301IsFollowedToFinalResource) {
        const TransferResult result = download("/old");

        EXPECT_TRUE(result.ok()) << "error message: " << result.error_message;
        EXPECT_EQ(result.http_status_code, 200) << "the result describes the final response";
        EXPECT_TRUE(read_file(part_path()) == content_);
    }

    // --- Network and disk errors -----------------------------------------------------------

    TEST_F(DownloadOnceTest, ClosedPortIsNetworkError) {
        // Start and stop a second server: its port is now free, so the connection is refused.
        dm::test::TestServer closed;
        closed.start();
        const std::string url = closed.url("/file");
        closed.stop();

        const TransferResult result = download_once(TransferRequest{.url = url, .part_path = part_path()});

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::Network);
        EXPECT_EQ(result.http_status_code, 0) << "no response was received";
        EXPECT_FALSE(result.error_message.empty()) << "the user needs to know why it failed";
    }

    TEST_F(DownloadOnceTest, UnwritablePathIsDiskWriteError) {
        const fs::path missing_dir = sandbox() / "no-such-dir";
        const TransferResult result = download_once(
                TransferRequest{.url = server_.url("/file"), .part_path = missing_dir / "file.bin.part"});

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::DiskWrite);
        EXPECT_FALSE(fs::exists(missing_dir)) << "download_once must not create directories";
    }
}
