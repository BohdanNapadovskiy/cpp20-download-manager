#include "core/state_store.hpp"
#include "test_printers.hpp"
#include "test_sandbox.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using dm::core::load_state;
using dm::core::LoadResult;
using dm::core::save_state;
using dm::core::SaveResult;
using dm::core::TaskInfo;
using dm::core::TaskState;

// Behaviour checked by these tests (step 4 of docs/IMPLEMENTATION_PLAN.md, README "Поведінка",
// "Стан між запусками"):
//   * saved tasks come back with id, URL, target path, state, total size, error message and
//     HTTP status; what is not stored (downloaded bytes, speed, attempt) comes back as 0;
//   * the If-Range validator is saved; a file written before it existed loads with an empty one;
//   * canceled tasks are not saved; Downloading and Retrying come back as Queued;
//   * non-ASCII (Cyrillic) paths survive, both in the tasks and in the path of the file itself;
//   * a missing file is a first start: success, no tasks, no message;
//   * a broken file (not JSON, cut short, unknown state) fails: no tasks and a message;
//   * saving creates the missing directory, replaces the old file, leaves no .tmp behind, and
//     a failed save is reported and keeps the previous file intact.
namespace {
    std::string read_file(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    void write_file(const fs::path& path, const std::string& content) {
        std::ofstream(path, std::ios::binary) << content;
    }

    class StateStoreTest : public dm::test::SandboxTest {
    protected:
        fs::path state_file() const { return sandbox() / "state.json"; }

        TaskInfo task(dm::core::TaskId id, TaskState state, const std::string& name) const {
            return TaskInfo{.id = id,
                            .url = "https://example.com/files/" + name,
                            .state = state,
                            .target_path = sandbox() / "downloads" / name};
        }

        // Saves, asserts success, and returns what loading the same file gives.
        LoadResult round_trip(const std::vector<TaskInfo>& tasks) {
            const SaveResult saved = save_state(state_file(), tasks);
            EXPECT_TRUE(saved.success) << saved.error_message;
            return load_state(state_file());
        }
    };

    // --- what is stored -----------------------------------------------------------------------

    TEST_F(StateStoreTest, SavedTasksAreLoadedBack) {
        TaskInfo failed = task(42, TaskState::Failed, "b.iso");
        failed.http_status_code = 404;
        failed.error_message = "download_once() failed: server responded with HTTP 404";
        TaskInfo paused = task(7, TaskState::Paused, "a.bin");
        paused.total_bytes = 1048576;

        const LoadResult loaded = round_trip({paused, task(9, TaskState::Completed, "c.zip"), failed});

        EXPECT_TRUE(loaded.success) << loaded.error_message;
        EXPECT_TRUE(loaded.error_message.empty()) << loaded.error_message;
        ASSERT_EQ(loaded.tasks.size(), 3u);

        EXPECT_EQ(loaded.tasks[0].id, 7u) << "ids are kept: the user refers to tasks by id";
        EXPECT_EQ(loaded.tasks[0].url, paused.url);
        EXPECT_EQ(loaded.tasks[0].target_path, paused.target_path);
        EXPECT_EQ(loaded.tasks[0].state, TaskState::Paused);
        EXPECT_EQ(loaded.tasks[0].total_bytes, 1048576u);

        EXPECT_EQ(loaded.tasks[1].id, 9u);
        EXPECT_EQ(loaded.tasks[1].state, TaskState::Completed);

        EXPECT_EQ(loaded.tasks[2].id, 42u);
        EXPECT_EQ(loaded.tasks[2].state, TaskState::Failed);
        EXPECT_EQ(loaded.tasks[2].http_status_code, 404);
        EXPECT_EQ(loaded.tasks[2].error_message, failed.error_message) << "a failed task still says why";
    }

    TEST_F(StateStoreTest, EveryKindOfStoredStateComesBack) {
        const LoadResult loaded = round_trip({task(1, TaskState::Queued, "1"), task(2, TaskState::Paused, "2"),
                                              task(3, TaskState::Failed, "3"), task(4, TaskState::Completed, "4")});

        ASSERT_EQ(loaded.tasks.size(), 4u);
        EXPECT_EQ(loaded.tasks[0].state, TaskState::Queued);
        EXPECT_EQ(loaded.tasks[1].state, TaskState::Paused);
        EXPECT_EQ(loaded.tasks[2].state, TaskState::Failed);
        EXPECT_EQ(loaded.tasks[3].state, TaskState::Completed);
    }

    TEST_F(StateStoreTest, RunTimeFieldsAreNotStored) {
        TaskInfo paused = task(1, TaskState::Paused, "a.bin");
        paused.downloaded_bytes = 400 * 1024;
        paused.speed_bps = 5000;
        paused.attempt = 3;

        const LoadResult loaded = round_trip({paused});

        ASSERT_EQ(loaded.tasks.size(), 1u);
        EXPECT_EQ(loaded.tasks[0].downloaded_bytes, 0u) << "the size of the .part decides, not the file";
        EXPECT_EQ(loaded.tasks[0].speed_bps, 0u);
        EXPECT_EQ(loaded.tasks[0].attempt, 0u) << "after a restart the attempts start again";
    }

    TEST_F(StateStoreTest, ValidatorIsSavedAndLoaded) {
        TaskInfo paused = task(1, TaskState::Paused, "a.bin");
        paused.validator = "\"v1-abc\"";
        TaskInfo dated = task(2, TaskState::Paused, "b.bin");
        dated.validator = "Wed, 21 Oct 2025 07:28:00 GMT";

        const LoadResult loaded = round_trip({paused, dated});

        ASSERT_EQ(loaded.tasks.size(), 2u);
        EXPECT_EQ(loaded.tasks[0].validator, "\"v1-abc\"") << "quotes included";
        EXPECT_EQ(loaded.tasks[1].validator, "Wed, 21 Oct 2025 07:28:00 GMT");
    }

    // A state.json written before the validator existed has no such key. Losing every task
    // after an update of the program would be far worse than not checking If-Range once.
    TEST_F(StateStoreTest, FileWithoutValidatorStillLoads) {
        write_file(state_file(), R"({"version": 1, "tasks": [{"id": 3, "url": "https://example.com/a.bin",
            "target_path": "a.bin", "state": "Paused", "total_bytes": 100, "error_message": "",
            "http_status_code": 0}]})");

        const LoadResult loaded = load_state(state_file());

        EXPECT_TRUE(loaded.success) << loaded.error_message;
        ASSERT_EQ(loaded.tasks.size(), 1u);
        EXPECT_EQ(loaded.tasks[0].id, 3u);
        EXPECT_TRUE(loaded.tasks[0].validator.empty());
    }

    TEST_F(StateStoreTest, EmptyListIsSavedAndLoaded) {
        const LoadResult loaded = round_trip({});

        EXPECT_TRUE(loaded.success) << "an empty list is a valid state, not a broken file";
        EXPECT_TRUE(loaded.tasks.empty());
        EXPECT_TRUE(loaded.error_message.empty());
    }

    // --- rules ---------------------------------------------------------------------------------

    TEST_F(StateStoreTest, CanceledTasksAreNotSaved) {
        const LoadResult loaded = round_trip({task(1, TaskState::Queued, "a.bin"),
                                              task(2, TaskState::Canceled, "b.bin"),
                                              task(3, TaskState::Completed, "c.bin")});

        ASSERT_EQ(loaded.tasks.size(), 2u);
        EXPECT_EQ(loaded.tasks[0].id, 1u);
        EXPECT_EQ(loaded.tasks[1].id, 3u);
    }

    TEST_F(StateStoreTest, DownloadingAndRetryingComeBackAsQueued) {
        const LoadResult loaded = round_trip({task(1, TaskState::Downloading, "a.bin"),
                                              task(2, TaskState::Retrying, "b.bin")});

        ASSERT_EQ(loaded.tasks.size(), 2u);
        EXPECT_EQ(loaded.tasks[0].state, TaskState::Queued) << "the program stopped in the middle of it";
        EXPECT_EQ(loaded.tasks[1].state, TaskState::Queued);
    }

    // --- non-ASCII paths ------------------------------------------------------------------------

    TEST_F(StateStoreTest, CyrillicTargetPathSurvives) {
        TaskInfo info = task(1, TaskState::Queued, "x");
        info.target_path = sandbox() / fs::path(u8"завантаження") / fs::path(u8"звіт за рік.pdf");

        const LoadResult loaded = round_trip({info});

        ASSERT_EQ(loaded.tasks.size(), 1u);
        EXPECT_EQ(loaded.tasks[0].target_path, info.target_path)
                << "path::string() would turn the Cyrillic letters into '?' on Windows";
    }

    TEST_F(StateStoreTest, StateFileInCyrillicDirectoryWorks) {
        const fs::path dir = sandbox() / fs::path(u8"стан");
        fs::create_directories(dir);
        const fs::path file = dir / "state.json";

        const SaveResult saved = save_state(file, {task(1, TaskState::Queued, "a.bin")});
        ASSERT_TRUE(saved.success) << saved.error_message;
        const LoadResult loaded = load_state(file);

        EXPECT_TRUE(loaded.success) << loaded.error_message;
        EXPECT_EQ(loaded.tasks.size(), 1u);
    }

    // --- missing and broken files --------------------------------------------------------------

    TEST_F(StateStoreTest, MissingFileIsAFirstStart) {
        const LoadResult loaded = load_state(state_file());

        EXPECT_TRUE(loaded.success) << "no file yet is not an error";
        EXPECT_TRUE(loaded.tasks.empty());
        EXPECT_TRUE(loaded.error_message.empty());
    }

    TEST_F(StateStoreTest, FileThatIsNotJsonIsReportedAsBroken) {
        write_file(state_file(), "this is not json {");

        const LoadResult loaded = load_state(state_file());

        EXPECT_FALSE(loaded.success);
        EXPECT_TRUE(loaded.tasks.empty());
        EXPECT_FALSE(loaded.error_message.empty()) << "the user must be told that the state was lost";
    }

    TEST_F(StateStoreTest, EmptyFileIsReportedAsBroken) {
        write_file(state_file(), "");

        const LoadResult loaded = load_state(state_file());

        EXPECT_FALSE(loaded.success);
        EXPECT_TRUE(loaded.tasks.empty());
        EXPECT_FALSE(loaded.error_message.empty());
    }

    TEST_F(StateStoreTest, FileCutShortIsReportedAsBroken) {
        ASSERT_TRUE(save_state(state_file(), {task(1, TaskState::Queued, "a.bin"),
                                              task(2, TaskState::Paused, "b.bin")}).success);
        const std::string whole = read_file(state_file());
        write_file(state_file(), whole.substr(0, whole.size() / 2));

        const LoadResult loaded = load_state(state_file());

        EXPECT_FALSE(loaded.success);
        EXPECT_TRUE(loaded.tasks.empty()) << "half a file is not restored half-way";
        EXPECT_FALSE(loaded.error_message.empty());
    }

    // Relies on the state being stored by its name ("Queued"), as the plan for the file format says.
    TEST_F(StateStoreTest, UnknownStateIsReportedAsBroken) {
        ASSERT_TRUE(save_state(state_file(), {task(1, TaskState::Queued, "a.bin")}).success);
        std::string content = read_file(state_file());
        const auto at = content.find("\"Queued\"");
        ASSERT_NE(at, std::string::npos) << "the state must be stored by its name, e.g. \"Queued\"";
        content.replace(at, 8, "\"Sleeping\"");
        write_file(state_file(), content);

        const LoadResult loaded = load_state(state_file());

        EXPECT_FALSE(loaded.success);
        EXPECT_TRUE(loaded.tasks.empty());
        EXPECT_FALSE(loaded.error_message.empty());
    }

    // --- saving ---------------------------------------------------------------------------------

    TEST_F(StateStoreTest, SaveCreatesMissingDirectory) {
        const fs::path file = sandbox() / "downloads" / "state.json";

        const SaveResult saved = save_state(file, {task(1, TaskState::Queued, "a.bin")});

        EXPECT_TRUE(saved.success) << saved.error_message;
        EXPECT_TRUE(fs::exists(file)) << "./downloads may not exist on the first start";
    }

    TEST_F(StateStoreTest, SavingAgainReplacesTheFile) {
        ASSERT_TRUE(save_state(state_file(), {task(1, TaskState::Queued, "a.bin")}).success);

        const LoadResult loaded = round_trip({task(2, TaskState::Paused, "b.bin")});

        ASSERT_EQ(loaded.tasks.size(), 1u) << "the old file must be replaced, not kept or appended to";
        EXPECT_EQ(loaded.tasks[0].id, 2u);
    }

    TEST_F(StateStoreTest, NoTemporaryFileIsLeftBehind) {
        ASSERT_TRUE(save_state(state_file(), {task(1, TaskState::Queued, "a.bin")}).success);

        EXPECT_FALSE(fs::exists(sandbox() / "state.json.tmp"));
    }

    TEST_F(StateStoreTest, FailedSaveIsReported) {
        write_file(sandbox() / "not-a-dir", "a file where a directory should be");
        const fs::path file = sandbox() / "not-a-dir" / "state.json";

        const SaveResult saved = save_state(file, {task(1, TaskState::Queued, "a.bin")});

        EXPECT_FALSE(saved.success);
        EXPECT_FALSE(saved.error_message.empty());
    }

    // The .tmp is a directory here, so it cannot be written: a save that fails half-way must
    // leave the previous state.json as it was.
    TEST_F(StateStoreTest, FailedSaveKeepsThePreviousFile) {
        ASSERT_TRUE(save_state(state_file(), {task(1, TaskState::Queued, "a.bin")}).success);
        const std::string before = read_file(state_file());
        fs::create_directories(sandbox() / "state.json.tmp" / "blocker");

        const SaveResult saved = save_state(state_file(), {task(2, TaskState::Paused, "b.bin")});

        EXPECT_FALSE(saved.success);
        EXPECT_EQ(read_file(state_file()), before) << "the old state must survive a failed save";
    }
}
