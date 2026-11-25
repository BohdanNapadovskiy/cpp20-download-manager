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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using dm::core::DownloadManager;
using dm::core::RetryPolicy;
using dm::core::TaskId;
using dm::core::TaskInfo;
using dm::core::TaskState;
using dm::core::TransferError;
using dm::core::TransferResult;
using dm::test::FakeExecutor;

// Behaviour checked by these tests (steps 2, 3.4 and 3.5 of docs/IMPLEMENTATION_PLAN.md, README
// "Поведінка", docs/Table.md). The executor is a fake: no network, the test decides when a
// download ends, and a held download reacts to its stop token like download_once.
//   * add() returns a new id; the task is visible in snapshot() with its URL and target path;
//   * the executor gets part_path = target_path + ".part"; on success the manager renames it
//     to target_path and the task becomes Completed; on failure it becomes Failed and keeps
//     the error message and HTTP status;
//   * never more downloads at once than worker threads, and every worker is used;
//   * pause/cancel work on queued tasks; resume puts a paused or failed task at the END of the
//     queue with all its attempts and without the old error message;
//   * a running download can be paused (-> Paused, .part kept, resume downloads it again) or
//     canceled (-> Canceled, .part deleted); other running downloads are not affected;
//   * a forbidden action (unknown id, wrong state) returns false and changes nothing;
//   * snapshot() shows the progress reported by the executor (downloaded / total size, 0 = not
//     known yet), separately for every task; a new attempt starts again from zero;
//   * snapshot() shows the speed while downloading and 0 in every other state; a resumed
//     download does not count the bytes already in its .part as speed;
//   * a temporary failure (network, HTTP 5xx/408/429) is retried up to max_attempts times
//     with retry_delay() between attempts; meanwhile the task is Retrying and shows its
//     attempt number and the last error; other failures are not retried; when the attempts
//     run out the task is Failed with the last error; pause, cancel and the destructor
//     interrupt the wait at once; resume gives the task all its attempts again;
//   * the state listener gets a full snapshot, sorted by id, after every change of a task's
//     state: add, pause/resume/cancel of a waiting task, and the worker's Downloading, Retrying
//     and final states; a refused action reports nothing; a pause or cancel of a running task
//     is reported when the worker reaches the new state; the last snapshot the listener gets
//     is the final state; the listener may call snapshot() and may throw;
//   * restore() puts back the tasks loaded from state.json with their ids: queued ones go to the
//     queue in the given order, paused, failed and completed ones only to the list; Downloading
//     and Retrying come back as Queued, Canceled is skipped; downloaded bytes come from the
//     .part (a completed task shows its full size); speed and attempt start from 0; the next
//     add() gets an id after the largest restored one; a task whose id exists is not restored
//     again; restore() returns how many tasks it added;
//   * the destructor stops and joins every worker, whether idle or busy; it interrupts running
//     downloads instead of waiting for them, and starts no queued task.
namespace {
    constexpr auto timeout = std::chrono::seconds(5);

    using namespace std::chrono_literals;

    // Most tests are about a single attempt: a failure becomes Failed at once. The retry tests
    // pass their own policy.
    RetryPolicy no_retries() {
        return {.max_attempts = 1};
    }

    // Retries without noticeable waiting.
    RetryPolicy quick_retries(std::uint32_t attempts) {
        return {.max_attempts = attempts, .initial_delay = 1ms, .max_delay = 1ms};
    }

    // A wait between attempts far longer than any test timeout: whatever ends it within a test
    // is an interruption, not the delay running out.
    RetryPolicy endless_wait(std::uint32_t attempts = 5) {
        return {.max_attempts = attempts, .initial_delay = 10min, .max_delay = 10min};
    }

    // Collects every snapshot the manager reports. The manager may call it from several threads.
    class ListenerRecorder {
    public:
        DownloadManager::StateListener listener() {
            return [this](const std::vector<TaskInfo>& tasks) {
                std::scoped_lock lock(mutex_);
                calls_.push_back(tasks);
            };
        }

        std::size_t count() const {
            std::scoped_lock lock(mutex_);
            return calls_.size();
        }

        std::vector<std::vector<TaskInfo>> calls() const {
            std::scoped_lock lock(mutex_);
            return calls_;
        }

        // The last snapshot reported so far; empty optional if there was none.
        std::optional<std::vector<TaskInfo>> last() const {
            std::scoped_lock lock(mutex_);
            if (calls_.empty()) {
                return std::nullopt;
            }
            return calls_.back();
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::vector<TaskInfo>> calls_;
    };

    // The task with `id` in a snapshot, if it is there.
    std::optional<TaskInfo> task_in(const std::vector<TaskInfo>& tasks, TaskId id) {
        for (const TaskInfo& info : tasks) {
            if (info.id == id) {
                return info;
            }
        }
        return std::nullopt;
    }

    std::optional<TaskState> state_in(const std::vector<TaskInfo>& tasks, TaskId id) {
        const auto info = task_in(tasks, id);
        return info ? std::optional<TaskState>(info->state) : std::nullopt;
    }

    TransferResult network_error(const std::string& message = "fake: connection reset") {
        return {.error = TransferError::Network, .error_message = message};
    }

    TransferResult http_error(int status) {
        return {.error = TransferError::HttpStatus, .http_status_code = status,
                .error_message = "fake: server responded with HTTP " + std::to_string(status)};
    }

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

        DownloadManager& start(std::size_t threads, RetryPolicy policy = no_retries(),
                               DownloadManager::StateListener listener = {}) {
            manager_.emplace(fake_.executor(), threads, policy, std::move(listener));
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

        // Starts one held download of "a.bin" and makes its speed non-zero: a first progress
        // report, a real pause of just over a second, then a second report with more bytes.
        // The second report stays below the full size, so the final report of a completed
        // download keeps the measured speed instead of restarting the meter.
        TaskId start_with_measurable_speed() {
            fake_.block();
            start(1);
            const TaskId id = add("a.bin");
            EXPECT_TRUE(fake_.wait_started(1));
            EXPECT_TRUE(wait_until(id, [](const TaskInfo& info) { return info.downloaded_bytes > 0; }));

            std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // one full speed window
            fake_.report_held(FakeExecutor::content_for(url("a.bin")).size() - 1);

            EXPECT_TRUE(wait_until(id, [](const TaskInfo& info) { return info.speed_bps > 0; }))
                    << "the manager must pass progress to its SpeedMeter";
            return id;
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

        // Polls until the last reported snapshot satisfies `condition`; false after the timeout.
        bool wait_for_report(const std::function<bool(const std::vector<TaskInfo>&)>& condition) const {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto last = recorder_.last();
                if (last && condition(*last)) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }

        bool wait_for_reported_state(TaskId id, TaskState expected) const {
            return wait_for_report([id, expected](const std::vector<TaskInfo>& tasks) {
                return state_in(tasks, id) == expected;
            });
        }

        // A manager without workers: tasks stay where add/pause/resume/cancel put them, so the
        // reports of these actions can be checked one by one.
        // A task as load_state() gives it back: what restore() receives.
        TaskInfo saved(TaskId id, TaskState state, const std::string& name) const {
            return TaskInfo{.id = id, .url = url(name), .state = state, .target_path = target(name)};
        }

        DownloadManager& start_without_workers() {
            return start(0, no_retries(), recorder_.listener());
        }

        FakeExecutor fake_;
        ListenerRecorder recorder_;   // declared before manager_: outlives the workers that call it
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

    // --- resume of a failed task ------------------------------------------------------------

    TEST_F(DownloadManagerTest, FailedTaskCanBeResumedAndCompleted) {
        fake_.fail_next(url("a.bin"), network_error());
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));

        EXPECT_TRUE(manager_->resume(id)) << "the user may retry a failed task by hand";
        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 2u);
        EXPECT_EQ(read_file(target("a.bin")), FakeExecutor::content_for(url("a.bin")));
    }

    TEST_F(DownloadManagerTest, ResumedFailedTaskForgetsTheOldError) {
        fake_.fail_next(url("a.bin"), http_error(503));
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        ASSERT_EQ(find(id)->http_status_code, 503);

        fake_.block();   // keep the new attempt running, so its result cannot overwrite the fields
        ASSERT_TRUE(manager_->resume(id));
        const auto info = find(id);
        EXPECT_TRUE(info->error_message.empty()) << "a queued task must not show the last error";
        EXPECT_EQ(info->http_status_code, 0);
    }

    TEST_F(DownloadManagerTest, ResumedPausedTaskForgetsTheStopMessage) {
        fake_.block();
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));
        ASSERT_FALSE(find(id)->error_message.empty()) << "precondition: the abort left a message";

        ASSERT_TRUE(manager_->resume(id));
        EXPECT_TRUE(find(id)->error_message.empty());
    }

    TEST_F(DownloadManagerTest, ResumedFailedTaskGetsAllAttemptsAgain) {
        start(1, quick_retries(2));
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        ASSERT_EQ(fake_.calls_for(url("fail")), 2u);

        ASSERT_TRUE(manager_->resume(id));
        ASSERT_TRUE(wait_until(id, [this](const TaskInfo& info) {
            return info.state == TaskState::Failed && fake_.calls_for(url("fail")) >= 3;
        }));
        EXPECT_EQ(fake_.calls_for(url("fail")), 4u) << "two more attempts after the resume";
        EXPECT_EQ(find(id)->attempt, 2u);
    }

    TEST_F(DownloadManagerTest, CanceledTaskCannotBeResumed) {
        fake_.block();
        start(1);
        add("a.bin");
        const TaskId canceled = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));
        ASSERT_TRUE(manager_->cancel(canceled));

        EXPECT_FALSE(manager_->resume(canceled)) << "Canceled is a final state";
        EXPECT_EQ(state(canceled), TaskState::Canceled);
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

    // --- speed (3.4b) ----------------------------------------------------------------------
    // The manager measures speed with the real clock over windows of 1 s, so a non-zero speed
    // needs two progress reports more than a second apart: these tests wait once, just over a
    // second. The speed arithmetic itself is tested without waiting in speed_meter_test.cpp.

    TEST_F(DownloadManagerTest, NewTaskHasNoSpeed) {
        fake_.block();
        start(1);
        add("a.bin");
        const TaskId waiting = add("b.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        EXPECT_EQ(find(waiting)->speed_bps, 0u);
    }

    TEST_F(DownloadManagerTest, SpeedIsVisibleWhileDownloading) {
        const TaskId id = start_with_measurable_speed();

        const auto info = find(id);
        EXPECT_EQ(info->state, TaskState::Downloading);
        EXPECT_GT(info->speed_bps, 0u) << "two progress reports over a second apart give a speed";
        EXPECT_LE(info->speed_bps, FakeExecutor::content_for(url("a.bin")).size())
                << "fewer bytes than the whole body arrived in more than a second";
    }

    TEST_F(DownloadManagerTest, CompletedTaskHasZeroSpeed) {
        const TaskId id = start_with_measurable_speed();

        fake_.release();
        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_EQ(find(id)->speed_bps, 0u) << "a finished download is not downloading at any speed";
    }

    // A resumed download reports the bytes already in the .part from its first report on
    // (download_once counts the whole file). Those bytes did not arrive during this attempt,
    // so they must not show up as speed: the meter starts from the first report of the attempt.
    TEST_F(DownloadManagerTest, SpeedAfterResumeCountsOnlyNewBytes) {
        constexpr std::size_t already_on_disk = 400 * 1024;
        constexpr std::size_t file_total = 1024 * 1024;
        constexpr std::size_t new_bytes = 10 * 1024;
        fake_.block();
        fake_.set_report_partial(false);   // the first report below is the attempt's first one
        start(1);
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_held(1)) << "report_held() reaches only calls already held";

        fake_.report_held(already_on_disk, file_total);
        ASSERT_TRUE(wait_until(id, [](const TaskInfo& info) { return info.downloaded_bytes == already_on_disk; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // one full speed window
        fake_.report_held(already_on_disk + new_bytes, file_total);

        ASSERT_TRUE(wait_until(id, [](const TaskInfo& info) { return info.speed_bps > 0; }));
        EXPECT_LE(find(id)->speed_bps, new_bytes)
                << "only " << new_bytes << " bytes arrived in more than a second; the "
                << already_on_disk << " bytes from the .part are not speed";
    }

    TEST_F(DownloadManagerTest, PausedTaskHasZeroSpeed) {
        const TaskId id = start_with_measurable_speed();

        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));
        EXPECT_EQ(find(id)->speed_bps, 0u) << "a paused download is not downloading at any speed";
    }

    // --- retries (3.5) ---------------------------------------------------------------------

    TEST_F(DownloadManagerTest, FirstAttemptHasNumberOne) {
        start(1, quick_retries(5));
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_EQ(find(id)->attempt, 1u);
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 1u) << "a success is not repeated";
    }

    TEST_F(DownloadManagerTest, NetworkFailureIsRetriedUntilSuccess) {
        fake_.fail_next(url("a.bin"), network_error());
        fake_.fail_next(url("a.bin"), network_error());
        start(1, quick_retries(5));
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 3u) << "two failures, then a success";
        EXPECT_EQ(find(id)->attempt, 3u);
        EXPECT_EQ(read_file(target("a.bin")), FakeExecutor::content_for(url("a.bin")));
        EXPECT_TRUE(find(id)->error_message.empty()) << "a completed task shows no old error";
    }

    TEST_F(DownloadManagerTest, ServerErrorsAreRetried) {
        fake_.fail_next(url("a.bin"), http_error(503));
        fake_.fail_next(url("a.bin"), http_error(429));
        start(1, quick_retries(5));
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 3u);
    }

    TEST_F(DownloadManagerTest, NotFoundIsNotRetried) {
        start(1, quick_retries(5));
        const TaskId id = add("404");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        EXPECT_EQ(fake_.calls_for(url("404")), 1u) << "another attempt cannot fix a 404";
        EXPECT_EQ(find(id)->attempt, 1u);
    }

    TEST_F(DownloadManagerTest, TlsErrorIsNotRetried) {
        fake_.fail_next(url("a.bin"), {.error = TransferError::Tls, .error_message = "fake: bad certificate"});
        start(1, quick_retries(5));
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 1u);
    }

    TEST_F(DownloadManagerTest, TaskFailsWhenAttemptsRunOut) {
        start(1, quick_retries(3));
        const TaskId id = add("fail");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        EXPECT_EQ(fake_.calls_for(url("fail")), 3u) << "max_attempts counts every attempt, the first too";
        EXPECT_EQ(find(id)->attempt, 3u);
        EXPECT_FALSE(find(id)->error_message.empty());
    }

    TEST_F(DownloadManagerTest, FailedTaskKeepsTheLastError) {
        fake_.fail_next(url("a.bin"), network_error("fake: first error"));
        fake_.fail_next(url("a.bin"), http_error(503));
        start(1, quick_retries(2));
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Failed));
        const auto info = find(id);
        EXPECT_EQ(info->error_message, "fake: server responded with HTTP 503");
        EXPECT_EQ(info->http_status_code, 503);
    }

    TEST_F(DownloadManagerTest, TaskIsRetryingWhileWaitingForNextAttempt) {
        start(1, endless_wait());
        const TaskId id = add("fail");

        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));
        const auto info = find(id);
        EXPECT_EQ(info->attempt, 1u) << "the attempt that has just failed";
        EXPECT_FALSE(info->error_message.empty()) << "the user sees why the task is retrying";
        EXPECT_EQ(info->speed_bps, 0u);
        EXPECT_EQ(fake_.calls_for(url("fail")), 1u) << "the next attempt waits for the delay";
    }

    TEST_F(DownloadManagerTest, NextAttemptWaitsForTheDelay) {
        fake_.fail_next(url("a.bin"), network_error());
        start(1, {.max_attempts = 2, .initial_delay = 300ms, .max_delay = 300ms});
        const auto started = std::chrono::steady_clock::now();
        const TaskId id = add("a.bin");

        ASSERT_TRUE(wait_for_state(id, TaskState::Completed));
        EXPECT_GE(std::chrono::steady_clock::now() - started, 300ms);
    }

    TEST_F(DownloadManagerTest, RetryingTaskCanBePausedAtOnce) {
        start(1, endless_wait());
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));

        EXPECT_TRUE(manager_->pause(id));
        EXPECT_TRUE(wait_for_state(id, TaskState::Paused)) << "pause must not wait for the delay";
        EXPECT_EQ(fake_.calls_for(url("fail")), 1u) << "a paused task makes no more attempts";
    }

    TEST_F(DownloadManagerTest, RetryingTaskCanBeCanceledAtOnce) {
        start(1, endless_wait());
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));

        EXPECT_TRUE(manager_->cancel(id));
        EXPECT_TRUE(wait_for_state(id, TaskState::Canceled)) << "cancel must not wait for the delay";
        EXPECT_EQ(fake_.calls_for(url("fail")), 1u);
    }

    TEST_F(DownloadManagerTest, RetryingTaskCannotBeResumed) {
        start(1, endless_wait());
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));

        EXPECT_FALSE(manager_->resume(id)) << "only a paused task can be resumed";
        EXPECT_EQ(state(id), TaskState::Retrying);
    }

    TEST_F(DownloadManagerTest, ResumeGivesAllAttemptsAgain) {
        start(1, endless_wait(2));
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));
        ASSERT_TRUE(manager_->pause(id));
        ASSERT_TRUE(wait_for_state(id, TaskState::Paused));

        ASSERT_TRUE(manager_->resume(id));
        // Without the reset this second call would be attempt 2 of 2 and the task would fail.
        ASSERT_TRUE(wait_until(id, [this](const TaskInfo& info) {
            return fake_.calls_for(url("fail")) == 2 &&
                   (info.state == TaskState::Retrying || info.state == TaskState::Failed);
        }));
        const auto info = find(id);
        EXPECT_EQ(info->state, TaskState::Retrying) << "a resumed task starts its attempts again";
        EXPECT_EQ(info->attempt, 1u);
    }

    TEST_F(DownloadManagerTest, QueuedTaskStartsWhileAnotherIsRetrying) {
        start(2, endless_wait());
        const TaskId retrying = add("fail");
        ASSERT_TRUE(wait_for_state(retrying, TaskState::Retrying));

        // The waiting worker must not swallow the wake-up meant for the idle one.
        const TaskId next = add("a.bin");
        EXPECT_TRUE(wait_for_state(next, TaskState::Completed)) << "the second worker is free";
    }

    TEST_F(DownloadManagerTest, DestructorInterruptsTheWaitBetweenAttempts) {
        start(1, endless_wait());
        const TaskId id = add("fail");
        ASSERT_TRUE(wait_for_state(id, TaskState::Retrying));

        EXPECT_TRUE(finishes_in_time([this] { manager_.reset(); }))
                << "closing the program must not wait for the delay";
    }

    // --- state listener (step 4, saving the state) ------------------------------------------

    TEST_F(DownloadManagerTest, AddIsReported) {
        start_without_workers();
        const TaskId id = add("a.bin");

        ASSERT_EQ(recorder_.count(), 1u);
        const auto last = recorder_.last();
        EXPECT_EQ(state_in(*last, id), TaskState::Queued);
        EXPECT_EQ(task_in(*last, id)->url, url("a.bin"));
    }

    TEST_F(DownloadManagerTest, EveryReportIsTheWholeList) {
        start_without_workers();
        add("a.bin");
        add("b.bin");
        add("c.bin");

        ASSERT_EQ(recorder_.count(), 3u);
        EXPECT_EQ(recorder_.last()->size(), 3u) << "the listener saves the whole list, not one task";
    }

    TEST_F(DownloadManagerTest, PauseResumeAndCancelOfWaitingTaskAreReported) {
        start_without_workers();
        const TaskId id = add("a.bin");

        ASSERT_TRUE(manager_->pause(id));
        EXPECT_EQ(recorder_.count(), 2u);
        EXPECT_EQ(state_in(*recorder_.last(), id), TaskState::Paused);

        ASSERT_TRUE(manager_->resume(id));
        EXPECT_EQ(recorder_.count(), 3u);
        EXPECT_EQ(state_in(*recorder_.last(), id), TaskState::Queued);

        ASSERT_TRUE(manager_->cancel(id));
        EXPECT_EQ(recorder_.count(), 4u);
        EXPECT_EQ(state_in(*recorder_.last(), id), TaskState::Canceled);
    }

    TEST_F(DownloadManagerTest, CancelOfPausedTaskIsReported) {
        start_without_workers();
        const TaskId id = add("a.bin");
        ASSERT_TRUE(manager_->pause(id));

        ASSERT_TRUE(manager_->cancel(id));
        EXPECT_EQ(state_in(*recorder_.last(), id), TaskState::Canceled);
    }

    TEST_F(DownloadManagerTest, RefusedActionsReportNothing) {
        start_without_workers();
        const TaskId id = add("a.bin");
        const std::size_t before = recorder_.count();

        EXPECT_FALSE(manager_->pause(id + 100));
        EXPECT_FALSE(manager_->resume(id + 100));
        EXPECT_FALSE(manager_->cancel(id + 100));
        EXPECT_FALSE(manager_->resume(id)) << "a queued task cannot be resumed";

        EXPECT_EQ(recorder_.count(), before) << "nothing changed, so there is nothing to save";
    }

    TEST_F(DownloadManagerTest, ReportsAndSnapshotAreSortedById) {
        start_without_workers();
        std::vector<TaskId> ids;
        for (int i = 0; i < 20; ++i) {
            ids.push_back(add("file" + std::to_string(i) + ".bin"));
        }

        // Kept in a variable: a range-for over *recorder_.last() would read a destroyed temporary.
        const auto last = recorder_.last();
        ASSERT_TRUE(last.has_value());
        std::vector<TaskId> reported;
        for (const TaskInfo& info : *last) {
            reported.push_back(info.id);
        }
        EXPECT_EQ(reported, ids) << "list and state.json show the tasks in the order they were added";

        std::vector<TaskId> listed;
        for (const TaskInfo& info : manager_->snapshot()) {
            listed.push_back(info.id);
        }
        EXPECT_EQ(listed, ids);
    }

    TEST_F(DownloadManagerTest, WorkerReportsDownloadingAndCompleted) {
        fake_.block();
        start(1, no_retries(), recorder_.listener());
        const TaskId id = add("a.bin");

        EXPECT_TRUE(wait_for_reported_state(id, TaskState::Downloading)) << "the worker took the task";
        fake_.release();
        EXPECT_TRUE(wait_for_reported_state(id, TaskState::Completed));
    }

    TEST_F(DownloadManagerTest, FailureIsReported) {
        start(1, no_retries(), recorder_.listener());
        const TaskId id = add("404");

        ASSERT_TRUE(wait_for_reported_state(id, TaskState::Failed));
        EXPECT_EQ(task_in(*recorder_.last(), id)->http_status_code, 404) << "the saved task says why it failed";
    }

    TEST_F(DownloadManagerTest, RetryingIsReportedBeforeTheWait) {
        start(1, endless_wait(), recorder_.listener());
        const TaskId id = add("fail");

        EXPECT_TRUE(wait_for_reported_state(id, TaskState::Retrying))
                << "the delay is 10 minutes: the report must not wait for it";
    }

    TEST_F(DownloadManagerTest, PauseOfRunningTaskIsReportedWhenTheWorkerStops) {
        fake_.block();
        start(1, no_retries(), recorder_.listener());
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        ASSERT_TRUE(manager_->pause(id));
        EXPECT_TRUE(wait_for_reported_state(id, TaskState::Paused));
    }

    TEST_F(DownloadManagerTest, CancelOfRunningTaskIsReportedWhenTheWorkerStops) {
        fake_.block();
        start(1, no_retries(), recorder_.listener());
        const TaskId id = add("a.bin");
        ASSERT_TRUE(fake_.wait_started(1));

        ASSERT_TRUE(manager_->cancel(id));
        EXPECT_TRUE(wait_for_reported_state(id, TaskState::Canceled));
    }

    // Several workers report at the same time; an older snapshot must never be the last one,
    // or the saved file would show a state the tasks have already left.
    TEST_F(DownloadManagerTest, LastReportIsTheFinalStateWithSeveralWorkers) {
        start(3, no_retries(), recorder_.listener());
        std::vector<TaskId> ids;
        for (int i = 0; i < 30; ++i) {
            ids.push_back(add(i % 3 == 0 ? "fail" : i % 3 == 1 ? "404" : "file" + std::to_string(i) + ".bin"));
        }
        for (const TaskId id : ids) {
            ASSERT_TRUE(wait_until(id, [](const TaskInfo& info) {
                return info.state == TaskState::Completed || info.state == TaskState::Failed;
            }));
        }

        const auto final_state = manager_->snapshot();
        EXPECT_TRUE(wait_for_report([&final_state](const std::vector<TaskInfo>& reported) {
            if (reported.size() != final_state.size()) {
                return false;
            }
            for (std::size_t i = 0; i < reported.size(); ++i) {
                if (reported[i].id != final_state[i].id || reported[i].state != final_state[i].state) {
                    return false;
                }
            }
            return true;
        })) << "the last snapshot the listener got must match the final state";
    }

    // A listener that calls back into the manager must not dead-lock: it runs without the lock.
    TEST_F(DownloadManagerTest, ListenerMayCallSnapshot) {
        std::size_t seen = 0;
        start(0, no_retries(), [this, &seen](const std::vector<TaskInfo>&) {
            seen = manager_->snapshot().size();
        });

        EXPECT_TRUE(finishes_in_time([this] { (void)manager_->add(url("a.bin"), target("a.bin")); }))
                << "the listener was called while the manager held its lock";
        EXPECT_EQ(seen, 1u);
    }

    TEST_F(DownloadManagerTest, ThrowingListenerDoesNotBreakTheManager) {
        start(1, no_retries(), [](const std::vector<TaskInfo>&) {
            throw std::runtime_error("cannot write state.json");
        });

        const TaskId id = add("a.bin");
        EXPECT_TRUE(wait_for_state(id, TaskState::Completed)) << "the worker survives the listener's exception";
        EXPECT_TRUE(fs::exists(target("a.bin")));
    }

    // --- restore (step 4, loading the state) ----------------------------------------------

    TEST_F(DownloadManagerTest, RestoredTasksKeepTheirIdsAndStates) {
        start(0);
        const std::size_t restored = manager_->restore({saved(7, TaskState::Paused, "a.bin"),
                                                        saved(9, TaskState::Completed, "b.bin"),
                                                        saved(42, TaskState::Failed, "c.bin")});

        EXPECT_EQ(restored, 3u);
        const auto tasks = manager_->snapshot();
        ASSERT_EQ(tasks.size(), 3u);
        EXPECT_EQ(tasks[0].id, 7u) << "the user refers to tasks by id, so ids survive a restart";
        EXPECT_EQ(tasks[0].state, TaskState::Paused);
        EXPECT_EQ(tasks[0].url, url("a.bin"));
        EXPECT_EQ(tasks[0].target_path, target("a.bin"));
        EXPECT_EQ(tasks[1].id, 9u);
        EXPECT_EQ(tasks[1].state, TaskState::Completed);
        EXPECT_EQ(tasks[2].id, 42u);
        EXPECT_EQ(tasks[2].state, TaskState::Failed);
    }

    TEST_F(DownloadManagerTest, NextAddGetsAnIdAfterTheLargestRestored) {
        start(0);
        ASSERT_EQ(manager_->restore({saved(7, TaskState::Paused, "a.bin"), saved(42, TaskState::Failed, "b.bin")}), 2u);

        const TaskId id = add("c.bin");

        EXPECT_EQ(id, 43u);
        EXPECT_EQ(manager_->snapshot().size(), 3u) << "the new task must not be lost on a clashing id";
    }

    TEST_F(DownloadManagerTest, RestoredQueuedTasksAreDownloadedInTheirOrder) {
        start(1);
        ASSERT_EQ(manager_->restore({saved(1, TaskState::Queued, "a.bin"), saved(2, TaskState::Queued, "b.bin"),
                                     saved(3, TaskState::Queued, "c.bin")}), 3u);

        for (const TaskId id : {1u, 2u, 3u}) {
            ASSERT_TRUE(wait_for_state(id, TaskState::Completed)) << "task " << id;
        }
        const auto requests = fake_.requests();
        ASSERT_EQ(requests.size(), 3u);
        EXPECT_EQ(requests[0].url, url("a.bin"));
        EXPECT_EQ(requests[1].url, url("b.bin"));
        EXPECT_EQ(requests[2].url, url("c.bin"));
    }

    TEST_F(DownloadManagerTest, RestoredQueuedTaskWakesAWorker) {
        start(2);   // the workers are already waiting when restore() fills the queue

        ASSERT_EQ(manager_->restore({saved(1, TaskState::Queued, "a.bin"), saved(2, TaskState::Queued, "b.bin")}), 2u);

        EXPECT_TRUE(wait_for_state(1, TaskState::Completed));
        EXPECT_TRUE(wait_for_state(2, TaskState::Completed));
    }

    TEST_F(DownloadManagerTest, RestoredPausedTaskWaitsUntilResumed) {
        start(1);
        ASSERT_EQ(manager_->restore({saved(1, TaskState::Paused, "a.bin"), saved(2, TaskState::Queued, "b.bin")}), 2u);
        ASSERT_TRUE(wait_for_state(2, TaskState::Completed));
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 0u) << "a paused task is not downloaded on its own";

        ASSERT_TRUE(manager_->resume(1));
        EXPECT_TRUE(wait_for_state(1, TaskState::Completed));
    }

    TEST_F(DownloadManagerTest, RestoredFailedTaskCanBeResumed) {
        start(1);
        TaskInfo failed = saved(1, TaskState::Failed, "a.bin");
        failed.http_status_code = 503;
        failed.error_message = "download_once() failed: server responded with HTTP 503";
        ASSERT_EQ(manager_->restore({failed}), 1u);
        EXPECT_EQ(find(1)->error_message, failed.error_message) << "the reason survives the restart";

        ASSERT_TRUE(manager_->resume(1));
        EXPECT_TRUE(wait_for_state(1, TaskState::Completed));
    }

    TEST_F(DownloadManagerTest, DownloadingAndRetryingAreRestoredAsQueued) {
        start(0);
        ASSERT_EQ(manager_->restore({saved(1, TaskState::Downloading, "a.bin"),
                                     saved(2, TaskState::Retrying, "b.bin")}), 2u);

        EXPECT_EQ(state(1), TaskState::Queued) << "no worker is running it after a restart";
        EXPECT_EQ(state(2), TaskState::Queued);
        EXPECT_FALSE(manager_->resume(1)) << "a queued task is already waiting for a worker";
    }

    TEST_F(DownloadManagerTest, RestoredDownloadingTaskIsDownloadedAgain) {
        start(1);
        ASSERT_EQ(manager_->restore({saved(1, TaskState::Downloading, "a.bin")}), 1u);

        EXPECT_TRUE(wait_for_state(1, TaskState::Completed)) << "it went back to the queue";
    }

    TEST_F(DownloadManagerTest, CanceledTaskIsNotRestored) {
        start(0);
        const std::size_t restored = manager_->restore({saved(1, TaskState::Paused, "a.bin"),
                                                        saved(2, TaskState::Canceled, "b.bin")});

        EXPECT_EQ(restored, 1u);
        ASSERT_EQ(manager_->snapshot().size(), 1u);
        EXPECT_EQ(manager_->snapshot()[0].id, 1u);
    }

    TEST_F(DownloadManagerTest, RestoredProgressComesFromThePartFile) {
        start(0);
        std::ofstream(target("a.bin").string() + ".part", std::ios::binary) << std::string(400, 'x');
        TaskInfo with_part = saved(1, TaskState::Paused, "a.bin");
        with_part.total_bytes = 1000;
        with_part.downloaded_bytes = 999;   // what the file says does not count: the .part decides
        TaskInfo without_part = saved(2, TaskState::Paused, "b.bin");
        without_part.total_bytes = 1000;
        TaskInfo completed = saved(3, TaskState::Completed, "c.bin");
        completed.total_bytes = 1000;

        ASSERT_EQ(manager_->restore({with_part, without_part, completed}), 3u);

        EXPECT_EQ(find(1)->downloaded_bytes, 400u) << "the size of the .part on disk";
        EXPECT_EQ(find(1)->total_bytes, 1000u);
        EXPECT_EQ(find(2)->downloaded_bytes, 0u) << "no .part, nothing downloaded yet";
        EXPECT_EQ(find(3)->downloaded_bytes, 1000u) << "a completed task shows N of N";
    }

    TEST_F(DownloadManagerTest, RestoredTaskStartsWithoutSpeedOrAttempts) {
        start(0);
        TaskInfo info = saved(1, TaskState::Paused, "a.bin");
        info.speed_bps = 5000;
        info.attempt = 3;

        ASSERT_EQ(manager_->restore({info}), 1u);

        EXPECT_EQ(find(1)->speed_bps, 0u);
        EXPECT_EQ(find(1)->attempt, 0u);
    }

    TEST_F(DownloadManagerTest, RestoringTheSameTasksAgainAddsNothing) {
        start(0);
        const std::vector<TaskInfo> tasks{saved(1, TaskState::Queued, "a.bin"), saved(2, TaskState::Paused, "b.bin")};
        ASSERT_EQ(manager_->restore(tasks), 2u);

        EXPECT_EQ(manager_->restore(tasks), 0u) << "a task whose id exists is not restored twice";
        EXPECT_EQ(manager_->snapshot().size(), 2u);
    }

    TEST_F(DownloadManagerTest, RestoredQueuedTaskIsQueuedOnce) {
        start(1);
        const std::vector<TaskInfo> tasks{saved(1, TaskState::Queued, "a.bin")};
        ASSERT_EQ(manager_->restore(tasks), 1u);
        (void)manager_->restore(tasks);

        ASSERT_TRUE(wait_for_state(1, TaskState::Completed));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // a second queue entry would start now
        EXPECT_EQ(fake_.calls_for(url("a.bin")), 1u) << "a skipped task must not be put in the queue again";
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
