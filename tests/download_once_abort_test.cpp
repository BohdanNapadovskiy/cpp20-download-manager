#include "core/http_transfer.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stop_token>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using dm::core::download_once;
using dm::core::TransferError;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 3.1 of docs/IMPLEMENTATION_PLAN.md):
//   * request.stop_token interrupts a running download: the result is TransferError::Aborted,
//     never success, and it comes back promptly instead of after the whole file;
//   * the interruption works even while the server sends nothing (curl's progress callback
//     runs without incoming data);
//   * bytes received before the stop stay in the .part file, exactly as sent: resuming
//     (step 4) continues from them;
//   * a token that is already stopped aborts before anything is downloaded.
namespace {
    constexpr std::size_t pattern_size = 1024 * 1024;        // content repeats with this period
    constexpr std::size_t chunk_size = 16 * 1024;            // pattern_size is a multiple of it
    constexpr std::size_t endless_size = 1024 * pattern_size; // 1 GiB announced, never fully sent
    constexpr std::size_t stall_after = 64 * 1024;           // /stall sends this much, then waits
    constexpr auto abort_deadline = 3s;  // curl's progress callback runs at least once a second

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::uintmax_t size_or_zero(const fs::path& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    // Routes:
    //   /file     200, 1 MiB, sent at once
    //   /endless  200, announces 1 GiB, sends 16 KiB every few ms until the client goes away
    //   /stall    200, announces 1 GiB, sends 64 KiB and then sends nothing
    // The slow routes poll stopping_, so TearDown can end them before stopping the server.
    class DownloadOnceAbortTest : public dm::test::SandboxTest {
    protected:
        void SetUp() override {
            SandboxTest::SetUp();

            auto& server = server_.server();
            server.Get("/file", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(pattern_, "application/octet-stream");
            });
            server.Get("/endless", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content_provider(
                        endless_size, "application/octet-stream",
                        [this](std::size_t offset, std::size_t, httplib::DataSink& sink) {
                            if (stopping_) {
                                return false;
                            }
                            const bool written =
                                    sink.write(pattern_.data() + offset % pattern_size, chunk_size);
                            std::this_thread::sleep_for(2ms);
                            return written;
                        });
            });
            server.Get("/stall", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content_provider(
                        endless_size, "application/octet-stream",
                        [this](std::size_t offset, std::size_t, httplib::DataSink& sink) {
                            if (offset < stall_after) {
                                return sink.write(pattern_.data() + offset, chunk_size);
                            }
                            while (!stopping_ && sink.is_writable()) {
                                std::this_thread::sleep_for(10ms);
                            }
                            return false;
                        });
            });
            server_.start();
        }

        void TearDown() override {
            stopping_ = true;   // release handlers still sending or stalling
            server_.stop();
            SandboxTest::TearDown();
        }

        fs::path part_path() const { return sandbox() / "file.bin.part"; }

        // Starts download_once on another thread with the test's stop token.
        std::future<TransferResult> start_download(const std::string& path) {
            TransferRequest request{.url = server_.url(path), .part_path = part_path(),
                                    .stop_token = stop_.get_token()};
            return std::async(std::launch::async, [request] { return download_once(request); });
        }

        // Waits for the download to finish. On a timeout the slow routes are released first:
        // otherwise the future's destructor would wait for a 1 GiB body and hang the test run.
        bool finishes_in_time(std::future<TransferResult>& download, std::chrono::seconds timeout) {
            if (download.wait_for(timeout) == std::future_status::ready) {
                return true;
            }
            stopping_ = true;
            return false;
        }

        // Waits until the .part file holds at least `bytes`; false after a timeout.
        bool wait_for_part_size(std::uintmax_t bytes) const {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (size_or_zero(part_path()) >= bytes) {
                    return true;
                }
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }

        // The first `size` bytes of the (periodic) response body.
        std::string expected_prefix(std::size_t size) const {
            std::string expected;
            expected.reserve(size);
            while (expected.size() < size) {
                expected.append(pattern_, 0, std::min(pattern_size, size - expected.size()));
            }
            return expected;
        }

        const std::string pattern_ = dm::test::make_content(pattern_size);
        std::atomic<bool> stopping_{false};
        std::stop_source stop_;
        dm::test::TestServer server_;
    };

    // --- stop while data is flowing --------------------------------------------------------

    TEST_F(DownloadOnceAbortTest, StopDuringTransferIsAborted) {
        auto download = start_download("/endless");
        ASSERT_TRUE(wait_for_part_size(chunk_size)) << "the download never started";

        stop_.request_stop();
        ASSERT_TRUE(finishes_in_time(download, abort_deadline))
                << "download_once ignored the stop request";

        const TransferResult result = download.get();
        EXPECT_EQ(result.error, TransferError::Aborted);
        EXPECT_FALSE(result.ok()) << "an interrupted download must never be reported as success";
        EXPECT_FALSE(result.error_message.empty()) << "the user needs to know what happened";
        EXPECT_LT(result.bytes_transferred, endless_size);
    }

    TEST_F(DownloadOnceAbortTest, StopDuringTransferKeepsReceivedBytes) {
        auto download = start_download("/endless");
        ASSERT_TRUE(wait_for_part_size(chunk_size));

        stop_.request_stop();
        ASSERT_TRUE(finishes_in_time(download, abort_deadline));
        const TransferResult result = download.get();

        ASSERT_TRUE(fs::exists(part_path())) << "the received part must stay for resuming";
        const std::string written = read_file(part_path());
        EXPECT_EQ(written.size(), result.bytes_transferred);
        EXPECT_TRUE(written == expected_prefix(written.size()))
                << "the part file must hold exactly the bytes received before the stop";
    }

    // --- stop while the server sends nothing -----------------------------------------------

    TEST_F(DownloadOnceAbortTest, StopWhileServerIsSilentIsAborted) {
        auto download = start_download("/stall");
        ASSERT_TRUE(wait_for_part_size(stall_after)) << "the first chunk never arrived";

        stop_.request_stop();
        ASSERT_TRUE(finishes_in_time(download, abort_deadline))
                << "no data is flowing, so only the progress callback can notice the stop";

        const TransferResult result = download.get();
        EXPECT_EQ(result.error, TransferError::Aborted);
        EXPECT_EQ(result.bytes_transferred, stall_after);
        EXPECT_TRUE(read_file(part_path()) == expected_prefix(stall_after));
    }

    // --- stop before the start -------------------------------------------------------------

    TEST_F(DownloadOnceAbortTest, AlreadyStoppedTokenAbortsWithoutDownloading) {
        stop_.request_stop();

        auto download = start_download("/file");
        ASSERT_TRUE(finishes_in_time(download, abort_deadline));

        const TransferResult result = download.get();
        EXPECT_EQ(result.error, TransferError::Aborted);
        EXPECT_FALSE(result.ok());
        EXPECT_LT(result.bytes_transferred, pattern_size) << "the file must not be downloaded";
    }

    // --- a token nobody stops --------------------------------------------------------------

    TEST_F(DownloadOnceAbortTest, TokenThatIsNeverStoppedDoesNotDisturbDownload) {
        auto download = start_download("/file");
        ASSERT_TRUE(finishes_in_time(download, 5s));

        const TransferResult result = download.get();
        EXPECT_TRUE(result.ok()) << "error message: " << result.error_message;
        EXPECT_TRUE(read_file(part_path()) == pattern_);
    }
}
