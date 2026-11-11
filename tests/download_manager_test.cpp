#include "core/download_manager.hpp"
#include "fake_executor.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"

#include <gtest/gtest.h>

#include <chrono>
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

// Behaviour checked by these tests (step 2 of docs/IMPLEMENTATION_PLAN.md, README "Поведінка",
// docs/Table.md). The executor is a fake: no network, the test decides when a download ends.
//   * add() returns a new id; the task is visible in snapshot() with its URL and target path;
//   * the executor gets part_path = target_path + ".part"; on success the manager renames it
//     to target_path and the task becomes Completed; on failure it becomes Failed and keeps
//     the error message and HTTP status;
//   * never more downloads at once than worker threads, and every worker is used;
//   * pause/cancel work on queued tasks; resume puts a paused task at the END of the queue;
//   * a forbidden action (unknown id, wrong state) returns false and changes nothing;
//     in step 2 a running download cannot be paused or canceled yet (step 3 adds that);
//   * the destructor stops and joins every worker, whether idle or busy.
namespace {
    constexpr auto timeout = std::chrono::seconds(5);

    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // Runs `action` on another thread; true if it finished within the timeout. A hanging
    // action is detached so that the test reports a failure instead of hanging forever.
    bool finishes_in_time(std::function<void()> action) {
        std::promise<void> done;
        std::future<void> finished = done.get_future();
        std::thread worker([&action, &done] {
            action();
            done.set_value();
        });
        if (finished.wait_for(timeout) == std::future_status::ready) {
            worker.join();
            return true;
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
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto info = find(id);
                if (info && info->state == expected) {
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

    // Step 2 boundary: a running download cannot be interrupted yet. Step 3 (xferinfo callback)
    // will allow Downloading -> Paused / Canceled and these expectations will change.
    TEST_F(DownloadManagerTest, RunningTaskCannotBePausedOrCanceledYet) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(wait_for_state(id, TaskState::Downloading));

        EXPECT_FALSE(manager_->pause(id));
        EXPECT_FALSE(manager_->cancel(id));
        EXPECT_EQ(state(id), TaskState::Downloading);
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

        // The running download can only finish when the fake lets it go (step 2 cannot
        // interrupt it), so release it while the destructor is waiting for the workers.
        EXPECT_TRUE(finishes_in_time([this] {
            std::thread releaser([this] { fake_.release(); });
            manager_.reset();
            releaser.join();
        })) << "the destructor must stop the workers and join them";
    }
}
