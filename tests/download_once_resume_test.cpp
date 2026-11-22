#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using dm::core::download_once;
using dm::core::TransferError;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 4.1 of docs/IMPLEMENTATION_PLAN.md, README "Поведінка",
// "Продовження з місця"):
//   * without a .part file the whole file is downloaded and no Range is asked for;
//   * with a .part file of N bytes only the rest is asked for (Range: bytes=N-); a 206 answer
//     is appended to the .part, so it ends up equal to the whole file;
//   * progress counts the whole file: after a 206 the bytes already on disk are included in
//     both numbers, the last report is (file size, file size);
//   * bytes_transferred counts only the bytes received by this call;
//   * a server that refuses to resume (sends the whole file anyway, or answers 416) gives
//     TransferError::ResumeRejected, and the .part is deleted so the next attempt starts from 0;
//   * a download stopped half-way can be continued by the next call and gives the right file.
namespace {
    constexpr std::size_t file_size = 1024 * 1024;
    constexpr std::uint64_t part_size = 400 * 1024;
    constexpr std::size_t slow_chunk = 8 * 1024;

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    void write_file(const fs::path& path, const std::string& content) {
        std::ofstream(path, std::ios::binary) << content;
    }

    using Report = std::pair<std::uint64_t, std::uint64_t>;   // (downloaded, total)

    // Routes (all serve the same 1 MiB body):
    //   /file      honours Range: 206 with the rest of the file, 416 when the range starts at or
    //              past the end (cpp-httplib does this when the handler leaves the status alone)
    //   /no-range  ignores Range: always 200 with the whole file
    //   /slow      honours Range, sends 8 KiB pieces with a short pause, so a test can stop it
    // Every request records the Range header it carried ("" without one).
    class DownloadOnceResumeTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            auto& server = server_.server();
            server.Get("/file", [this](const httplib::Request& req, httplib::Response& res) {
                record(req);
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/no-range", [this](const httplib::Request& req, httplib::Response& res) {
                record(req);
                res.status = 200;   // an explicit 200 makes cpp-httplib send the whole body
                res.set_content(content_, "application/octet-stream");
            });
            server.Get("/slow", [this](const httplib::Request& req, httplib::Response& res) {
                record(req);
                res.set_content_provider(
                        content_.size(), "application/octet-stream",
                        [this](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                            if (stopping_) {
                                return false;
                            }
                            std::this_thread::sleep_for(2ms);
                            return sink.write(content_.data() + offset, std::min(slow_chunk, length));
                        });
            });
            server_.start();
        }

        void TearDown() override {
            stopping_ = true;
            server_.stop();
            SandboxTest::TearDown();
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        TransferRequest request_for(const std::string& path) {
            return TransferRequest{.url = server_.url(path), .part_path = part_path(),
                                   .on_progress = [this](std::uint64_t downloaded, std::uint64_t total) {
                                       reports_.emplace_back(downloaded, total);
                                   }};
        }

        TransferResult download(const std::string& path) { return download_once(request_for(path)); }

        // Range headers of all requests so far, in order.
        std::vector<std::string> ranges_seen() const {
            std::scoped_lock lock(mutex_);
            return ranges_seen_;
        }

        std::string prefix(std::uint64_t size) const { return content_.substr(0, size); }

        const std::string content_ = dm::test::make_content(file_size);
        std::vector<Report> reports_;   // filled on the downloading thread, read after it returns

    private:
        void record(const httplib::Request& req) {
            std::scoped_lock lock(mutex_);
            ranges_seen_.push_back(req.get_header_value("Range"));
        }

        dm::test::TestServer server_;
        mutable std::mutex mutex_;
        std::vector<std::string> ranges_seen_;
        std::atomic<bool> stopping_{false};
    };

    // --- no .part: nothing to continue ------------------------------------------------------

    TEST_F(DownloadOnceResumeTest, WithoutPartFileWholeFileIsDownloadedWithoutRange) {
        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 200);
        EXPECT_EQ(ranges_seen(), std::vector<std::string>{""}) << "nothing on disk, nothing to skip";
        EXPECT_TRUE(read_file(part_path()) == content_);
    }

    // --- .part present: continue ------------------------------------------------------------

    TEST_F(DownloadOnceResumeTest, ExistingPartAsksOnlyForTheRest) {
        write_file(part_path(), prefix(part_size));

        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.http_status_code, 206);
        EXPECT_EQ(ranges_seen(), std::vector<std::string>{"bytes=" + std::to_string(part_size) + "-"});
    }

    TEST_F(DownloadOnceResumeTest, RestIsAppendedToThePart) {
        write_file(part_path(), prefix(part_size));

        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << result.error_message;
        const std::string on_disk = read_file(part_path());
        EXPECT_EQ(on_disk.size(), file_size) << "the part must be appended to, not overwritten";
        EXPECT_TRUE(on_disk == content_) << "the joined file differs from the original";
    }

    TEST_F(DownloadOnceResumeTest, BytesTransferredCountsOnlyThisCall) {
        write_file(part_path(), prefix(part_size));

        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(result.bytes_transferred, file_size - part_size);
    }

    TEST_F(DownloadOnceResumeTest, ProgressCountsTheWholeFile) {
        write_file(part_path(), prefix(part_size));

        const TransferResult result = download("/file");

        ASSERT_TRUE(result.ok()) << result.error_message;
        ASSERT_FALSE(reports_.empty());
        EXPECT_EQ(reports_.back(), Report(file_size, file_size)) << "the last report is the whole file";
        for (const auto& [downloaded, total] : reports_) {
            if (total != 0) {
                EXPECT_EQ(total, file_size) << "total is the whole file, not just the rest";
                EXPECT_GE(downloaded, part_size) << "the bytes already on disk count as downloaded";
            } else {
                EXPECT_EQ(downloaded, 0u) << "before the answer arrives nothing is known yet";
            }
        }
    }

    // --- the server refuses to continue -----------------------------------------------------

    TEST_F(DownloadOnceResumeTest, ServerIgnoringRangeIsResumeRejected) {
        write_file(part_path(), prefix(part_size));

        const TransferResult result = download("/no-range");

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::ResumeRejected);
        EXPECT_FALSE(result.error_message.empty());
        EXPECT_FALSE(fs::exists(part_path())) << "the next attempt must start from zero";
    }

    TEST_F(DownloadOnceResumeTest, CompletePartIsAnsweredWith416AndResumeRejected) {
        write_file(part_path(), content_);   // the range would start at the end of the file

        const TransferResult result = download("/file");

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, TransferError::ResumeRejected);
        EXPECT_EQ(result.http_status_code, 416);
        EXPECT_FALSE(fs::exists(part_path()));
    }

    TEST_F(DownloadOnceResumeTest, PartLongerThanTheFileIsResumeRejected) {
        write_file(part_path(), content_ + "extra bytes the server does not have");

        const TransferResult result = download("/file");

        EXPECT_EQ(result.error, TransferError::ResumeRejected);
        EXPECT_FALSE(fs::exists(part_path()));
    }

    TEST_F(DownloadOnceResumeTest, AfterResumeRejectedNextCallDownloadsFromZero) {
        write_file(part_path(), prefix(part_size));
        ASSERT_EQ(download("/no-range").error, TransferError::ResumeRejected);

        const TransferResult result = download("/no-range");

        ASSERT_TRUE(result.ok()) << result.error_message;
        EXPECT_EQ(ranges_seen().back(), "") << "no part left, so no Range this time";
        EXPECT_TRUE(read_file(part_path()) == content_);
    }

    // --- stop half-way, then continue -------------------------------------------------------

    TEST_F(DownloadOnceResumeTest, StoppedDownloadIsContinuedByTheNextCall) {
        std::stop_source stop;
        std::atomic<std::uint64_t> received{0};
        TransferRequest first = request_for("/slow");
        first.stop_token = stop.get_token();
        first.on_progress = [&received](std::uint64_t downloaded, std::uint64_t) { received = downloaded; };
        auto running = std::async(std::launch::async, [first] { return download_once(first); });

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (received < 64 * 1024 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        stop.request_stop();
        ASSERT_EQ(running.wait_for(5s), std::future_status::ready);
        ASSERT_EQ(running.get().error, TransferError::Aborted) << "stopped before the end";
        const auto kept = fs::file_size(part_path());
        ASSERT_GT(kept, 0u);
        ASSERT_LT(kept, file_size);

        const TransferResult second = download("/slow");

        ASSERT_TRUE(second.ok()) << second.error_message;
        EXPECT_EQ(second.http_status_code, 206);
        EXPECT_EQ(ranges_seen().back(), "bytes=" + std::to_string(kept) + "-");
        EXPECT_TRUE(read_file(part_path()) == content_) << "the joined file differs from the original";
    }
}
