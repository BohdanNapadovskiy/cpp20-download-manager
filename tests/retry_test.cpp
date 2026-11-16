#include "core/retry.hpp"
#include "test_printers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

using namespace std::chrono_literals;
using dm::core::retry_delay;
using dm::core::RetryPolicy;
using dm::core::should_retry;
using dm::core::TransferError;
using dm::core::TransferResult;

// Behaviour checked by these tests (step 3.5 of docs/IMPLEMENTATION_PLAN.md, README "Поведінка"):
//   * should_retry: a temporary failure is worth another attempt (network, HTTP 5xx, 408, 429);
//     a failure that another attempt cannot fix is not (other 4xx, TLS, disk, abort, success);
//   * retry_delay: `attempt` is the attempt about to start (from 1); 0 before the first,
//     initial_delay before the second, doubling after that, never above max_delay; large
//     attempt numbers do not overflow;
//   * RetryPolicy defaults follow README: 5 attempts, 1 s, 30 s.
namespace {
    TransferResult http(int status) {
        return {.error = TransferError::HttpStatus, .http_status_code = status};
    }

    // --- should_retry: worth another attempt -----------------------------------------------

    TEST(ShouldRetryTest, NetworkErrorIsRetried) {
        EXPECT_TRUE(should_retry({.error = TransferError::Network}))
                << "a dropped connection, a timeout or a stalled download may work next time";
    }

    TEST(ShouldRetryTest, ServerErrorsAreRetried) {
        for (const int status : {500, 502, 503, 504, 599}) {
            EXPECT_TRUE(should_retry(http(status))) << "HTTP " << status;
        }
    }

    TEST(ShouldRetryTest, RequestTimeoutAndTooManyRequestsAreRetried) {
        EXPECT_TRUE(should_retry(http(408))) << "408 Request Timeout: try again later";
        EXPECT_TRUE(should_retry(http(429))) << "429 Too Many Requests: try again later";
    }

    // --- should_retry: not worth another attempt -------------------------------------------

    TEST(ShouldRetryTest, OtherClientErrorsAreNotRetried) {
        for (const int status : {400, 401, 403, 404, 410, 416}) {
            EXPECT_FALSE(should_retry(http(status))) << "HTTP " << status << " will not change";
        }
    }

    TEST(ShouldRetryTest, StatusesOutsideTheServerErrorRangeAreNotRetried) {
        EXPECT_FALSE(should_retry(http(499))) << "just below 5xx";
        EXPECT_FALSE(should_retry(http(600))) << "just above 5xx";
    }

    TEST(ShouldRetryTest, TlsDiskAndAbortAreNotRetried) {
        for (const auto error : {TransferError::Tls, TransferError::DiskWrite, TransferError::Aborted}) {
            EXPECT_FALSE(should_retry({.error = error})) << ::testing::PrintToString(error);
        }
    }

    TEST(ShouldRetryTest, SuccessIsNotRetried) {
        EXPECT_FALSE(should_retry({.error = TransferError::None, .http_status_code = 200}));
    }

    TEST(ShouldRetryTest, ServerErrorStatusWithoutHttpStatusErrorIsNotRetried) {
        // The status code counts only for an HttpStatus error: a 503 recorded next to a TLS
        // failure must not turn it into a retry.
        EXPECT_FALSE(should_retry({.error = TransferError::Tls, .http_status_code = 503}));
    }

    // --- retry_delay with the README defaults -------------------------------------------------

    TEST(RetryDelayTest, FirstAttemptStartsAtOnce) {
        EXPECT_EQ(retry_delay(RetryPolicy{}, 1), 0ms);
    }

    TEST(RetryDelayTest, DelayDoublesFromOneSecond) {
        const RetryPolicy policy{};
        EXPECT_EQ(retry_delay(policy, 2), 1s);
        EXPECT_EQ(retry_delay(policy, 3), 2s);
        EXPECT_EQ(retry_delay(policy, 4), 4s);
        EXPECT_EQ(retry_delay(policy, 5), 8s);
        EXPECT_EQ(retry_delay(policy, 6), 16s);
    }

    TEST(RetryDelayTest, DelayNeverExceedsMaximum) {
        const RetryPolicy policy{};
        EXPECT_EQ(retry_delay(policy, 7), 30s) << "32 s is capped at 30 s";
        EXPECT_EQ(retry_delay(policy, 8), 30s);
    }

    TEST(RetryDelayTest, LargeAttemptNumbersDoNotOverflow) {
        const RetryPolicy policy{};
        EXPECT_EQ(retry_delay(policy, 64), 30s);
        EXPECT_EQ(retry_delay(policy, 70), 30s) << "1 s * 2^68 would overflow if capped afterwards";
        EXPECT_EQ(retry_delay(policy, std::numeric_limits<std::uint32_t>::max()), 30s);
    }

    TEST(RetryDelayTest, AttemptZeroIsTreatedAsTheFirst) {
        EXPECT_EQ(retry_delay(RetryPolicy{}, 0), 0ms);
    }

    // --- retry_delay with a custom policy -----------------------------------------------------

    TEST(RetryDelayTest, CustomPolicyIsUsed) {
        const RetryPolicy policy{.initial_delay = 10ms, .max_delay = 50ms};
        EXPECT_EQ(retry_delay(policy, 2), 10ms);
        EXPECT_EQ(retry_delay(policy, 3), 20ms);
        EXPECT_EQ(retry_delay(policy, 4), 40ms);
        EXPECT_EQ(retry_delay(policy, 5), 50ms) << "80 ms is capped at 50 ms";
    }

    TEST(RetryDelayTest, InitialDelayAboveMaximumIsCapped) {
        const RetryPolicy policy{.initial_delay = 1min, .max_delay = 30s};
        EXPECT_EQ(retry_delay(policy, 2), 30s);
    }

    TEST(RetryDelayTest, ZeroInitialDelayMeansNoWaiting) {
        const RetryPolicy policy{.initial_delay = 0ms};
        EXPECT_EQ(retry_delay(policy, 2), 0ms);
        EXPECT_EQ(retry_delay(policy, 5), 0ms);
    }

    // --- defaults ---------------------------------------------------------------------------

    TEST(RetryPolicyTest, DefaultsFollowReadme) {
        const RetryPolicy policy{};
        EXPECT_EQ(policy.max_attempts, 5u);
        EXPECT_EQ(policy.initial_delay, 1s);
        EXPECT_EQ(policy.max_delay, 30s);
    }
}
