#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using dm::core::download_once;
using dm::core::TransferError;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 4, If-Range, README "Поведінка", "Продовження з місця"):
//   * when a .part is continued and request.if_range is not empty, the request carries
//     "If-Range: <if_range>", exactly as given;
//   * the file did not change (the validator matches) -> 206, the rest is appended;
//   * the file changed (the server sends the whole new file) -> ResumeRejected, the .part is
//     deleted, and the next call downloads the new file from zero;
//   * no If-Range without a .part (nothing is continued) or with an empty validator.
namespace {
    constexpr std::size_t file_size = 256 * 1024;
    constexpr std::size_t part_size = 100 * 1024;
    constexpr const char* old_etag = "\"v1\"";
    constexpr const char* new_etag = "\"v2\"";

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // One route, /file, that behaves like a real server with If-Range: it sends the current
    // version with its ETag; a Range request whose If-Range does not equal the current ETag gets
    // the whole file with 200 (cpp-httplib checks If-Range only for static files, so the handler
    // does it). Every request records the If-Range it carried (nullopt without one).
    class DownloadOnceIfRangeTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            server_.server().Get("/file", [this](const httplib::Request& req, httplib::Response& res) {
                std::scoped_lock lock(mutex_);
                if_range_seen_.push_back(req.has_header("If-Range")
                                                 ? std::optional<std::string>(req.get_header_value("If-Range"))
                                                 : std::nullopt);
                res.set_header("ETag", etag_);
                if (req.has_header("Range") && req.has_header("If-Range") &&
                    req.get_header_value("If-Range") != etag_) {
                    res.status = 200;   // validator does not match: ignore Range, send the new file
                }
                res.set_content(content_, "application/octet-stream");
            });
            server_.start();
        }

        void TearDown() override {
            server_.stop();
            SandboxTest::TearDown();
        }

        // Replaces the file on the server by a different version.
        void change_file_on_server() {
            std::scoped_lock lock(mutex_);
            content_ = dm::test::make_content(file_size, 7);
            etag_ = new_etag;
        }

        std::string content() const {
            std::scoped_lock lock(mutex_);
            return content_;
        }

        std::vector<std::optional<std::string>> if_range_seen() const {
            std::scoped_lock lock(mutex_);
            return if_range_seen_;
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        void write_part(const std::string& bytes) const { std::ofstream(part_path(), std::ios::binary) << bytes; }

        TransferResult download(const std::string& if_range) {
            return download_once(
                    TransferRequest{.url = server_.url("/file"), .part_path = part_path(), .if_range = if_range});
        }

        const std::string old_content_ = dm::test::make_content(file_size, 42);

    private:
        dm::test::TestServer server_;
        mutable std::mutex mutex_;
        std::string content_ = old_content_;
        std::string etag_ = old_etag;
        std::vector<std::optional<std::string>> if_range_seen_;
    };

    // --- the file did not change -----------------------------------------------------------

    TEST_F(DownloadOnceIfRangeTest, MatchingValidatorContinuesTheDownload) {
        write_part(old_content_.substr(0, part_size));

        const TransferResult result = download(old_etag);

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 206);
        EXPECT_TRUE(read_file(part_path()) == old_content_);
    }

    TEST_F(DownloadOnceIfRangeTest, IfRangeIsSentExactlyAsGiven) {
        write_part(old_content_.substr(0, part_size));

        (void)download(old_etag);

        ASSERT_EQ(if_range_seen().size(), 1u);
        EXPECT_EQ(if_range_seen()[0], std::optional<std::string>(old_etag)) << "quotes included";
    }

    TEST_F(DownloadOnceIfRangeTest, LastModifiedDateIsSentAsIfRangeToo) {
        write_part(old_content_.substr(0, part_size));
        const std::string date = "Wed, 21 Oct 2025 07:28:00 GMT";

        (void)download(date);

        ASSERT_EQ(if_range_seen().size(), 1u);
        EXPECT_EQ(if_range_seen()[0], std::optional<std::string>(date));
    }

    // --- the file changed on the server ----------------------------------------------------

    TEST_F(DownloadOnceIfRangeTest, ChangedFileIsResumeRejected) {
        write_part(old_content_.substr(0, part_size));
        change_file_on_server();

        const TransferResult result = download(old_etag);

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::ResumeRejected)
                << "joining the old part with the new file would corrupt it";
        EXPECT_FALSE(fs::exists(part_path())) << "the next attempt must start from zero";
    }

    TEST_F(DownloadOnceIfRangeTest, AfterAChangeTheNextCallGetsTheNewFile) {
        write_part(old_content_.substr(0, part_size));
        change_file_on_server();
        ASSERT_EQ(download(old_etag).error, TransferError::ResumeRejected);

        const TransferResult result = download(old_etag);

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 200);
        EXPECT_EQ(result.etag, new_etag) << "the new version's ETag, for the next resume";
        EXPECT_TRUE(read_file(part_path()) == content());
    }

    // --- when If-Range is not sent ---------------------------------------------------------

    TEST_F(DownloadOnceIfRangeTest, NoIfRangeWithoutAPart) {
        const TransferResult result = download(old_etag);

        ASSERT_TRUE(result.ok()) << result.error_message;
        ASSERT_EQ(if_range_seen().size(), 1u);
        EXPECT_EQ(if_range_seen()[0], std::nullopt) << "nothing is continued, so there is nothing to check";
    }

    TEST_F(DownloadOnceIfRangeTest, NoIfRangeWithAnEmptyValidator) {
        write_part(old_content_.substr(0, part_size));

        const TransferResult result = download("");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 206) << "the resume itself still works";
        ASSERT_EQ(if_range_seen().size(), 1u);
        EXPECT_EQ(if_range_seen()[0], std::nullopt);
    }
}
