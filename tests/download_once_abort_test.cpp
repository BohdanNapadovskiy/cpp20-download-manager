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
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using dm::core::download_once;
using dm::core::TransferError;
using dm::core::TransferRequest;
using dm::core::TransferResult;

// Behaviour checked by these tests (steps 3.1 and 3.3 of docs/IMPLEMENTATION_PLAN.md):
//   * request.stop_token interrupts a running download: the result is TransferError::Aborted,
//     never success, and it comes back promptly instead of after the whole file;
//   * the interruption works even while the server sends nothing (curl's progress callback
//     runs without incoming data);
//   * bytes received before the stop stay in the .part file, exactly as sent: resuming
//     (step 4) continues from them;
//   * a token that is already stopped aborts before anything is downloaded;
//   * a server that goes silent is cut after request.low_speed_time as TransferError::Network
//     (a network problem worth retrying, not an abort), keeping the received bytes;
//   * a fast download is never cut by the low-speed limit.
namespace {
    constexpr std::size_t pattern_size = 1024 * 1024;        // content repeats with this period
    constexpr std::size_t chunk_size = 16 * 1024;            // pattern_size is a multiple of it
    constexpr std::size_t endless_size = 1024 * pattern_size; // 1 GiB announced, never fully sent
    constexpr std::size_t stall_after = 64 * 1024;           // /stall sends this much, then waits
    constexpr auto abort_deadline = 3s;  // curl's progress callback runs at least once a second
    constexpr auto low_speed_deadline = 15s;   // see SilentServerTimesOutAsNetworkError

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
            return start_download(request_for(path));
        }

        std::future<TransferResult> start_download(TransferRequest request) {
            return std::async(std::launch::async,
                              [request = std::move(request)] { return download_once(request); });
        }

        // Every request reports its progress into received_, see wait_for_received().
        TransferRequest request_for(const std::string& path) {
            return TransferRequest{.url = server_.url(path), .part_path = part_path(),
                                   .stop_token = stop_.get_token(),
                                   .on_progress = [this](std::uint64_t downloaded, std::uint64_t) {
                                       received_ = downloaded;
                                   }};
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

        // Waits until download_once reports at least `bytes` received; false after a timeout.
        // Not the size of the .part file: the writer may keep the last bytes in its buffer until
        // the file is closed, and a server that stops sending never makes it write them out.
        bool wait_for_received(std::uint64_t bytes) const {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (received_ >= bytes) {
                    return true;
                }
                std::this_thread::sleep_for(1ms);
            }
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
        std::atomic<std::uint64_t> received_{0};   // last downloaded count from on_progress
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
        ASSERT_TRUE(wait_for_received(stall_after)) << "the first chunk never arrived";

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

    // --- low-speed timeout (3.3) -----------------------------------------------------------

    TEST_F(DownloadOnceAbortTest, SilentServerTimesOutAsNetworkError) {
        TransferRequest request = request_for("/stall");
        request.low_speed_time = 2s;   // nobody calls request_stop(): only the timeout can end it
        const auto start = std::chrono::steady_clock::now();
        auto download = start_download(std::move(request));

        // curl averages the speed over a window of about 5 s, so the first 64 KiB keep it above
        // the limit for a while: measured ~8 s in total. 15 s leaves room on a busy machine.
        ASSERT_TRUE(finishes_in_time(download, low_speed_deadline))
                << "a silent server must not hang the download";
        const auto elapsed = std::chrono::steady_clock::now() - start;

        const TransferResult result = download.get();
        EXPECT_EQ(result.error, TransferError::Network) << "too slow is a network problem, not an abort";
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(result.error_message.empty()) << "the user needs to know why it failed";
        EXPECT_GE(elapsed, 1s) << "cut too early: the limit allows 2 s of silence";
    }

    TEST_F(DownloadOnceAbortTest, SilentServerTimeoutKeepsReceivedBytes) {
        TransferRequest request = request_for("/stall");
        request.low_speed_time = 2s;
        auto download = start_download(std::move(request));
        ASSERT_TRUE(finishes_in_time(download, low_speed_deadline));

        const TransferResult result = download.get();
        EXPECT_EQ(result.bytes_transferred, stall_after);
        EXPECT_TRUE(read_file(part_path()) == expected_prefix(stall_after))
                << "the received part must stay for resuming";
    }

    TEST_F(DownloadOnceAbortTest, FastDownloadIsNotCutByLowSpeedLimit) {
        TransferRequest request = request_for("/file");
        request.low_speed_time = 1s;    // the shortest window curl accepts
        auto download = start_download(std::move(request));
        ASSERT_TRUE(finishes_in_time(download, 5s));

        const TransferResult result = download.get();
        EXPECT_TRUE(result.ok()) << "error message: " << result.error_message;
        EXPECT_TRUE(read_file(part_path()) == pattern_);
    }
}
