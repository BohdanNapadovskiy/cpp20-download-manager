#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using dm::core::download_once;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 4, If-Range, README "Поведінка", "Продовження з місця"):
//   * download_once reports the ETag and Last-Modified of the response exactly as the server sent
//     them, quotes and the W/ of a weak ETag included: that is what If-Range has to send back;
//   * header names are matched without regard to case (ETag, etag, ETAG are the same header);
//   * a response without them gives empty strings;
//   * after a redirect only the final response counts: the validators of the redirect itself
//     belong to another resource and must not be kept;
//   * a resumed download (206) reports them too.
namespace {
    constexpr const char* strong_etag = "\"v1-abc\"";
    constexpr const char* weak_etag = "W/\"v1-weak\"";
    constexpr const char* last_modified = "Wed, 21 Oct 2025 07:28:00 GMT";

    // Routes:
    //   /etag            ETag and Last-Modified, usual spelling
    //   /lowercase       the same headers written as "etag" and "last-modified"
    //   /uppercase       "ETAG" and "LAST-MODIFIED"
    //   /weak            a weak ETag only
    //   /plain           no validators at all
    //   /redirect-plain  302 with its own ETag and Last-Modified, to /plain
    //   /redirect-etag   302 with its own ETag, to /etag
    class DownloadOnceValidatorsTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            auto& server = server_.server();
            server.Get("/etag", [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("ETag", strong_etag);
                res.set_header("Last-Modified", last_modified);
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/lowercase", [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("etag", strong_etag);
                res.set_header("last-modified", last_modified);
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/uppercase", [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("ETAG", strong_etag);
                res.set_header("LAST-MODIFIED", last_modified);
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/weak", [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("ETag", weak_etag);
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/plain", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/redirect-plain", [](const httplib::Request&, httplib::Response& res) {
                res.set_header("ETag", "\"the-redirect\"");
                res.set_header("Last-Modified", "Mon, 01 Jan 2024 00:00:00 GMT");
                res.set_redirect("/plain", 302);
            });
            server.Get("/redirect-etag", [](const httplib::Request&, httplib::Response& res) {
                res.set_header("ETag", "\"the-redirect\"");
                res.set_redirect("/etag", 302);
            });
            server_.start();
        }

        void TearDown() override {
            server_.stop();
            SandboxTest::TearDown();
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        TransferResult download(const std::string& path) {
            return download_once(TransferRequest{.url = server_.url(path), .part_path = part_path()});
        }

        const std::string content_ = dm::test::make_content(64 * 1024);

    private:
        dm::test::TestServer server_;
    };

    TEST_F(DownloadOnceValidatorsTest, EtagAndLastModifiedAreReported) {
        const TransferResult result = download("/etag");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.etag, strong_etag) << "the quotes are part of the ETag";
        EXPECT_EQ(result.last_modified, last_modified);
    }

    TEST_F(DownloadOnceValidatorsTest, LowercaseHeaderNamesAreRecognised) {
        const TransferResult result = download("/lowercase");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.etag, strong_etag) << "HTTP header names do not depend on case";
        EXPECT_EQ(result.last_modified, last_modified);
    }

    TEST_F(DownloadOnceValidatorsTest, UppercaseHeaderNamesAreRecognised) {
        const TransferResult result = download("/uppercase");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.etag, strong_etag);
        EXPECT_EQ(result.last_modified, last_modified);
    }

    TEST_F(DownloadOnceValidatorsTest, WeakEtagIsReportedAsItIs) {
        const TransferResult result = download("/weak");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.etag, weak_etag) << "choosing what to send in If-Range is not download_once's job";
        EXPECT_TRUE(result.last_modified.empty());
    }

    TEST_F(DownloadOnceValidatorsTest, ResponseWithoutValidatorsGivesEmptyStrings) {
        const TransferResult result = download("/plain");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_TRUE(result.etag.empty()) << result.etag;
        EXPECT_TRUE(result.last_modified.empty()) << result.last_modified;
    }

    TEST_F(DownloadOnceValidatorsTest, ValidatorsOfTheRedirectAreNotKept) {
        const TransferResult result = download("/redirect-plain");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_TRUE(result.etag.empty()) << "\"the-redirect\" belongs to the redirect, not to the file";
        EXPECT_TRUE(result.last_modified.empty()) << result.last_modified;
    }

    TEST_F(DownloadOnceValidatorsTest, ValidatorsComeFromTheFinalResponse) {
        const TransferResult result = download("/redirect-etag");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.etag, strong_etag);
        EXPECT_EQ(result.last_modified, last_modified);
    }

    TEST_F(DownloadOnceValidatorsTest, ResumedDownloadReportsValidatorsToo) {
        std::ofstream(part_path(), std::ios::binary) << content_.substr(0, 1000);

        const TransferResult result = download("/etag");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 206);
        EXPECT_EQ(result.etag, strong_etag);
        EXPECT_EQ(result.last_modified, last_modified);
    }
}
