#include "core/download_manager.hpp"
#include "fake_executor.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using dm::core::DownloadManager;
using dm::core::TaskId;
using dm::core::TaskInfo;
using dm::core::TaskState;
using dm::test::FakeExecutor;

// Behaviour checked by these tests (steps 2 and 3.4 of docs/IMPLEMENTATION_PLAN.md, README
// "Поведінка", docs/Table.md). The executor is a fake: no network, the test decides when a
// download ends, and a held download reacts to its stop token like download_once.
//   * add() returns a new id; the task is visible in snapshot() with its URL and target path;
//   * the executor gets part_path = target_path + ".part"; on success the manager renames it
//     to target_path and the task becomes Completed; on failure it becomes Failed and keeps
//     the error message and HTTP status;
//   * never more downloads at once than worker threads, and every worker is used;
//   * pause/cancel work on queued tasks; resume puts a paused task at the END of the queue;
//   * a running download can be paused (-> Paused, .part kept, resume downloads it again) or
//     canceled (-> Canceled, .part deleted); other running downloads are not affected;
//   * a forbidden action (unknown id, wrong state) returns false and changes nothing;
//   * snapshot() shows the progress reported by the executor (downloaded / total size, 0 = not
//     known yet), separately for every task; a new attempt starts again from zero;
//   * the destructor stops and joins every worker, whether idle or busy; it interrupts running
//     downloads instead of waiting for them, and starts no queued task.
namespace {
    constexpr auto timeout = std::chrono::seconds(5);

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // Runs `action` on another thread; true if it finished within the timeout. On a timeout
    // `rescue` (if given) unblocks the action and the thread is joined, so the test fails cleanly
    // instead of leaving a thread that outlives the fixture. Without a rescue the hanging thread
    // is detached: a failure report is better than a test run that never ends.
    bool finishes_in_time(std::function<void()> action, const std::function<void()>& rescue = {}) {
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        std::thread worker([action = std::move(action), done] {
            action();
            done->set_value();
        });
        if (finished.wait_for(timeout) == std::future_status::ready) {
            worker.join();
            return true;
        }
        if (rescue) {
            rescue();
            worker.join();
            return false;
        }
        worker.detach();
        return false;
    }

    class DownloadManagerTest : public dm::test::SandboxTest {
    protected:
        void TearDown() override {
            fake_.release();      // never leave a worker blocked inside the fake
            manager_.reset();     // join the workers before the sandbox disappears
            SandboxTest::TearDown();
        }

        DownloadManager& start(std::size_t threads) {
            manager_.emplace(fake_.executor(), threads);
            return *manager_;
        }

        TaskId add(const std::string& name) {
            return manager_->add(url(name), target(name));
        }

        static std::string url(const std::string& name) { return "http://fake/" + name; }
        fs::path target(const std::string& name) const { return sandbox() / name; }

        std::optional<TaskInfo> find(TaskId id) const {
            for (TaskInfo& info : manager_->snapshot()) {
                if (info.id == id) {
                    return info;
                }
            }
            return std::nullopt;
        }

        TaskState state(TaskId id) const {
            const auto info = find(id);
            EXPECT_TRUE(info.has_value()) << "task " << id << " is missing from snapshot()";
            return info ? info->state : TaskState::Queued;
        }

        // Polls snapshot() until the task reaches `expected` (the state changes on a worker
        // thread). Returns false after the timeout instead of hanging.
        bool wait_for_state(TaskId id, TaskState expected) const {
            return wait_until(id, [expected](const TaskInfo& info) { return info.state == expected; });
        }

        // Polls snapshot() until the task satisfies `condition`; false after the timeout.
        bool wait_until(TaskId id, const std::function<bool(const TaskInfo&)>& condition) const {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto info = find(id);
                if (info && condition(*info)) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }

        FakeExecutor fake_;
        std::optional<DownloadManager> manager_;
    };

    // --- add / snapshot --------------------------------------------------------------------

    TEST_F(DownloadManagerTest, AddReturnsDistinctIds) {
        fake_.block();
        start(1);

        const TaskId first = add("a.bin");
        const TaskId second = add("b.bin");

        EXPECT_NE(first, second);
    }

    TEST_F(DownloadManagerTest, SnapshotShowsUrlAndTargetOfEachTask) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");

        const auto info = find(id);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->url, url("a.bin"));
        EXPECT_EQ(info->target_path, target("a.bin"));
    }

    TEST_F(DownloadManagerTest, TaskWaitsInQueueWhileAllWorkersAreBusy) {
        fake_.block();
        start(1);
        const TaskId running = add("a.bin");
        const TaskId waiting = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        EXPECT_TRUE(wait_for_state(running, TaskState::Downloading));
        EXPECT_EQ(state(waiting), TaskState::Queued);
    }

    // --- success and failure ---------------------------------------------------------------

    TEST_F(DownloadManagerTest, ExecutorWritesToPartPathNextToTarget) {
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));

        const auto requests = fake_.requests();
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(requests[0].url, url("a.bin"));
        EXPECT_EQ(requests[0].part_path, sandbox() / "a.bin.part");
    }

    TEST_F(DownloadManagerTest, SuccessfulTaskIsCompletedAndPartIsRenamed) {
        start(1);
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_TRUE(fs::exists(target("a.bin")));
        EXPECT_EQ(read_file(target("a.bin")), FakeExecutor::content_for(url("a.bin")));
        EXPECT_FALSE(fs::exists(sandbox() / "a.bin.part")) << "the .part file must be renamed";
        EXPECT_EQ(find(id)->http_status_code, 200);
    }

    TEST_F(DownloadManagerTest, FailedTaskKeepsErrorMessage) {
        start(1);
        const TaskId id = add("fail");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        EXPECT_FALSE(find(id)->error_message.empty()) << "the user needs to know why it failed";
        EXPECT_FALSE(fs::exists(target("fail")));
    }

    TEST_F(DownloadManagerTest, HttpErrorTaskIsFailedWithStatus) {
        start(1);
        const TaskId id = add("404");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        EXPECT_EQ(find(id)->http_status_code, 404);
    }

    // --- worker pool -----------------------------------------------------------------------

    TEST_F(DownloadManagerTest, ConcurrencyNeverExceedsThreadCount) {
        fake_.block();
        start(3);
        std::vector<TaskId> ids;
        for (int i = 0; i < 10; ++i) {
            ids.push_back(add("file" + std::to_string(i) + ".bin"));
        }
        ASSERT_TRUE(fake_.wait_started(3)) << "3 workers should start 3 downloads";

        fake_.release();
        for (const TaskId id : ids) {
            ASSERT_TRUE(wait_for_state(id, TaskState::Completed)) << "task " << id;
        }
        EXPECT_LE(fake_.max_active(), 3u);
    }

    TEST_F(DownloadManagerTest, EveryWorkerIsUsed) {
        fake_.block();
        start(3);
        for (int i = 0; i < 10; ++i) {
            add("file" + std::to_string(i) + ".bin");
        }

        ASSERT_TRUE(fake_.wait_started(3));
        EXPECT_EQ(fake_.max_active(), 3u) << "3 workers, 10 tasks: all 3 must download at once";
    }

    // --- pause / resume / cancel -----------------------------------------------------------

    TEST_F(DownloadManagerTest, PausedTaskIsNotDownloaded) {
        fake_.block();
        start(1);
        const TaskId running = add("a.bin");
        const TaskId paused = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        EXPECT_TRUE(manager_->pause(paused));
        EXPECT_EQ(state(paused), TaskState::Paused);

        fake_.release();
        ASSERT_TRUE(wait_for_state(running, TaskState::Completed));
        EXPECT_EQ(state(paused), TaskState::Paused);
        EXPECT_EQ(fake_.requests().size(), 1u) << "a paused task must not reach the executor";
    }

    TEST_F(DownloadManagerTest, ResumePutsTaskAtEndOfQueue) {
        fake_.block();
        start(1);
        const TaskId a = add("a.bin");
        const TaskId b = add("b.bin");
        const TaskId c = add("c.bin");
        ASSERT_TRUE(fake_.wait_started(1));   // a is running, b and c wait

        ASSERT_TRUE(manager_->pause(b));
        EXPECT_TRUE(manager_->resume(b));
        EXPECT_EQ(state(b), TaskState::Queued);

        fake_.release();
        ASSERT_TRUE(wait_for_state(a, TaskState::Completed));
        ASSERT_TRUE(wait_for_state(b, TaskState::Completed));
        ASSERT_TRUE(wait_for_state(c, TaskState::Completed));
        const auto requests = fake_.requests();
        ASSERT_EQ(requests.size(), 3u);
        EXPECT_EQ(requests[1].url, url("c.bin")) << "resumed b goes after c";
        EXPECT_EQ(requests[2].url, url("b.bin"));
    }

    TEST_F(DownloadManagerTest, CanceledTaskIsNotDownloaded) {
        fake_.block();
        start(1);
        const TaskId running = add("a.bin");
        const TaskId canceled = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        EXPECT_TRUE(manager_->cancel(canceled));
        EXPECT_EQ(state(canceled), TaskState::Canceled);

        fake_.release();
        ASSERT_TRUE(wait_for_state(running, TaskState::Completed));
        EXPECT_EQ(state(canceled), TaskState::Canceled);
        EXPECT_EQ(fake_.requests().size(), 1u) << "a canceled task must not reach the executor";
    }

    TEST_F(DownloadManagerTest, PausedTaskCanBeCanceled) {
        fake_.block();
        start(1);
        add("a.bin");
        const TaskId id = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(manager_->pause(id));

        EXPECT_TRUE(manager_->cancel(id));
        EXPECT_EQ(state(id), TaskState::Canceled);
    }

    // --- forbidden actions -----------------------------------------------------------------

    TEST_F(DownloadManagerTest, ActionsOnUnknownIdReturnFalse) {
        start(1);
        constexpr TaskId unknown = 12345;

        EXPECT_FALSE(manager_->pause(unknown));
        EXPECT_FALSE(manager_->resume(unknown));
        EXPECT_FALSE(manager_->cancel(unknown));
    }

    TEST_F(DownloadManagerTest, ResumeOfTaskThatIsNotPausedReturnsFalse) {
        fake_.block();
        start(1);
        add("a.bin");
        const TaskId queued = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        EXPECT_FALSE(manager_->resume(queued));
        EXPECT_EQ(state(queued), TaskState::Queued);
    }

    TEST_F(DownloadManagerTest, FinishedTasksCannotBePausedResumedOrCanceled) {
        start(1);
        const TaskId completed = add("a.bin");
        const TaskId canceled = add("b.bin");
        ASSERT_TRUE(wait_for_state(completed, TaskState::Completed));
        // b may already be done as well; cancel it only if it is still waiting.
        if (manager_->cancel(canceled)) {
            ASSERT_EQ(state(canceled), TaskState::Canceled);
            EXPECT_FALSE(manager_->pause(canceled));
            EXPECT_FALSE(manager_->resume(canceled));
            EXPECT_FALSE(manager_->cancel(canceled));
            EXPECT_EQ(state(canceled), TaskState::Canceled);
        }

        EXPECT_FALSE(manager_->pause(completed));
        EXPECT_FALSE(manager_->resume(completed));
        EXPECT_FALSE(manager_->cancel(completed));
        EXPECT_EQ(state(completed), TaskState::Completed);
    }

    // --- pause / cancel a running download (3.4) ---------------------------------------------

    TEST_F(DownloadManagerTest, RunningTaskCanBePaused) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));

        EXPECT_TRUE(manager_->pause(id));

        ASSERT_TRUE(wait_for_state(id, TaskState::Paused)) << "the download was not interrupted";
        EXPECT_EQ(fake_.aborted(), 1u);
        EXPECT_FALSE(fs::exists(target("a.bin")));
    }

    TEST_F(DownloadManagerTest, PausedRunningTaskKeepsPartFile) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));

        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));

        const fs::path part = sandbox() / "a.bin.part";
        ASSERT_TRUE(fs::exists(part)) << "pause keeps the .part file for resuming (README)";
        EXPECT_EQ(read_file(part), FakeExecutor::partial_content());
    }

    TEST_F(DownloadManagerTest, PausedRunningTaskCanBeResumedAndCompleted) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));
        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));

        ASSERT_TRUE(manager_->resume(id));
        ASSERT_TRUE(fake_.wait_started(2)) << "the resumed task must be downloaded again";
        fake_.release();

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed))
                << "a resumed task must not stay stopped: it needs a fresh stop source";
        EXPECT_EQ(fake_.requests().size(), 2u) << "exactly one more download after resume";
        EXPECT_EQ(fake_.aborted(), 1u) << "only the paused download was interrupted";
        EXPECT_EQ(read_file(target("a.bin")), FakeExecutor::content_for(url("a.bin")));
    }

    TEST_F(DownloadManagerTest, RunningTaskCanBeCanceled) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));

        EXPECT_TRUE(manager_->cancel(id));

        ASSERT_TRUE(wait_for_state(id, TaskState::Canceled)) << "the download was not interrupted";
        EXPECT_FALSE(fs::exists(sandbox() / "a.bin.part")) << "cancel deletes the .part file (README)";
        EXPECT_FALSE(fs::exists(target("a.bin")));
    }

    TEST_F(DownloadManagerTest, PausedRunningTaskCanBeCanceledAndLosesPartFile) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));
        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));
        ASSERT_TRUE(fs::exists(sandbox() / "a.bin.part"));

        EXPECT_TRUE(manager_->cancel(id));

        EXPECT_EQ(state(id), TaskState::Canceled);
        EXPECT_FALSE(fs::exists(sandbox() / "a.bin.part")) << "cancel deletes the .part file (README)";
    }

    TEST_F(DownloadManagerTest, PausingOneRunningTaskDoesNotStopOthers) {
        fake_.block();
        start(2);
        const TaskId paused = add("a.bin");
        const TaskId other = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(2));
        ASSERT_TRUE(wait_for_state(paused, TaskState::Downloading));
        ASSERT_TRUE(wait_for_state(other, TaskState::Downloading));

        ASSERT_TRUE(manager_->pause(paused));
        ASSERT_TRUE(wait_for_state(paused, TaskState::Paused));
        EXPECT_EQ(state(other), TaskState::Downloading) << "only the paused task is interrupted";

        fake_.release();
        EXPECT_TRUE(wait_for_state(other, TaskState::Completed));
        EXPECT_EQ(fake_.aborted(), 1u);
    }

    // --- progress (3.4b) ------------------------------------------------------------------

    TEST_F(DownloadManagerTest, NewTaskHasNoProgress) {
        fake_.block();
        start(1);
        add("a.bin");
        const TaskId waiting = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        const auto info = find(waiting);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->downloaded_bytes, 0u);
        EXPECT_EQ(info->total_bytes, 0u) << "0 means the size is not known yet";
    }

    TEST_F(DownloadManagerTest, ProgressIsVisibleWhileDownloading) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        const std::uint64_t partial = FakeExecutor::partial_content().size();
        const std::uint64_t full = FakeExecutor::content_for(url("a.bin")).size();
        EXPECT_TRUE(wait_until(id, [&](const TaskInfo& info) {
            return info.state == TaskState::Downloading && info.downloaded_bytes == partial;
        })) << "on_progress from the executor must reach snapshot() during the download";
        EXPECT_EQ(find(id)->total_bytes, full);
    }

    TEST_F(DownloadManagerTest, CompletedTaskShowsFullProgress) {
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));

        const std::uint64_t full = FakeExecutor::content_for(url("a.bin")).size();
        const auto info = find(id);
        EXPECT_EQ(info->downloaded_bytes, full);
        EXPECT_EQ(info->total_bytes, full);
    }

    TEST_F(DownloadManagerTest, ProgressIsKeptPerTask) {
        fake_.block();
        start(2);
        const TaskId small = add("a.bin");
        const TaskId large = add("a-much-longer-file-name.bin");   // longer URL -> larger fake body
        ASSERT_TRUE(fake_.wait_started(2));
        fake_.release();
        ASSERT_TRUE(wait_for_state(small, TaskState::Completed));
        ASSERT_TRUE(wait_for_state(large, TaskState::Completed));

        EXPECT_EQ(find(small)->total_bytes, FakeExecutor::content_for(url("a.bin")).size());
        EXPECT_EQ(find(large)->total_bytes,
                  FakeExecutor::content_for(url("a-much-longer-file-name.bin")).size())
                << "each task must get the progress of its own download";
    }

    TEST_F(DownloadManagerTest, ResumedTaskStartsProgressFromZero) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_until(id, [](const TaskInfo& info) { return info.downloaded_bytes > 0; }));
        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));

        fake_.set_report_partial(false);   // the second attempt reports nothing while held
        ASSERT_TRUE(manager_->resume(id));
        ASSERT_TRUE(fake_.wait_started(2));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));

        const auto info = find(id);
        EXPECT_EQ(info->downloaded_bytes, 0u) << "a new attempt starts from zero, not from the old numbers";
        EXPECT_EQ(info->total_bytes, 0u);
    }

    // --- shutdown --------------------------------------------------------------------------

    TEST_F(DownloadManagerTest, DestructorWithIdleWorkersReturns) {
        start(3);   // workers wait for tasks that never come

        EXPECT_TRUE(finishes_in_time([this] { manager_.reset(); }))
                << "idle workers must wake up on stop and be joined";
    }

    TEST_F(DownloadManagerTest, DestructorWithQueuedTasksReturns) {
        fake_.block();
        start(1);
        for (int i = 0; i < 5; ++i) {
            add("file" + std::to_string(i) + ".bin");
        }
        ASSERT_TRUE(fake_.wait_started(1));

        // Releases the fake while the destructor runs, so this test also passes for a manager
        // that waits for running downloads instead of interrupting them.
        EXPECT_TRUE(finishes_in_time(
                [this] {
                    std::thread releaser([this] { fake_.release(); });
                    manager_.reset();
                    releaser.join();
                },
                [this] { fake_.release(); }))
                << "the destructor must stop the workers and join them";
    }

    // Nobody releases the fake here: only the destructor's stop request can end the download.
    TEST_F(DownloadManagerTest, DestructorInterruptsRunningDownloadsAndStartsNoQueuedTask) {
        fake_.block();
        start(2);
        for (int i = 0; i < 5; ++i) {
            add("file" + std::to_string(i) + ".bin");
        }
        ASSERT_TRUE(fake_.wait_started(2));

        EXPECT_TRUE(finishes_in_time([this] { manager_.reset(); }, [this] { fake_.release(); }))
                << "the destructor must interrupt running downloads instead of waiting for them";
        EXPECT_EQ(fake_.aborted(), 2u) << "both running downloads were interrupted";
        EXPECT_EQ(fake_.requests().size(), 2u) << "no queued task may start during shutdown";
        EXPECT_TRUE(fs::exists(sandbox() / "file0.bin.part"))
                << "an interrupted download keeps its .part: it continues after a restart (step 4)";
    }
}
