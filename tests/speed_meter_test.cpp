#include "core/speed_meter.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using dm::core::SpeedMeter;

// Behaviour checked by these tests (step 3.4b of docs/IMPLEMENTATION_PLAN.md). Time is passed in,
// so every test uses made-up time points: no waiting, no real clock.
//   * the speed is measured over windows of at least 1 s: inside a window the previous value
//     is kept, a window of exactly 1 s counts;
//   * speed = bytes received in the window / real length of the window (2 s -> divide by 2);
//   * no new bytes for a full window -> 0;
//   * before the first full window, and for an update at the very start time -> 0;
//   * fewer bytes than before (a new attempt from zero) -> starts measuring again, speed 0;
//   * start() resets the meter; the initial byte count of start() is not counted as speed.
namespace {
    const SpeedMeter::TimePoint t0{};

    SpeedMeter started_at_zero() {
        SpeedMeter meter;
        meter.start(t0);
        return meter;
    }

    // --- first window ----------------------------------------------------------------------

    TEST(SpeedMeterTest, NewMeterHasZeroSpeed) {
        const SpeedMeter meter;
        EXPECT_EQ(meter.current_speed(), 0u);
    }

    TEST(SpeedMeterTest, NoSpeedBeforeFirstFullSecond) {
        SpeedMeter meter = started_at_zero();
        EXPECT_EQ(meter.update(500, t0 + 500ms), 0u) << "half a window is not enough to measure";
    }

    TEST(SpeedMeterTest, UpdateAtStartTimeIsZeroNotDivisionByZero) {
        SpeedMeter meter = started_at_zero();
        EXPECT_EQ(meter.update(100, t0), 0u);
    }

    TEST(SpeedMeterTest, OneSecondWindowGivesBytesPerSecond) {
        SpeedMeter meter = started_at_zero();
        EXPECT_EQ(meter.update(1000, t0 + 1s), 1000u) << "a window of exactly 1 s counts";
        EXPECT_EQ(meter.current_speed(), 1000u);
    }

    TEST(SpeedMeterTest, LongerWindowIsDividedByItsRealLength) {
        SpeedMeter meter = started_at_zero();
        EXPECT_EQ(meter.update(3000, t0 + 2s), 1500u) << "3000 bytes in 2 s is 1500 bytes/s";
    }

    TEST(SpeedMeterTest, FractionalSecondsAreNotTruncated) {
        SpeedMeter meter = started_at_zero();
        EXPECT_EQ(meter.update(3000, t0 + 1500ms), 2000u) << "1.5 s must not be rounded to 1 s";
    }

    // --- following windows -----------------------------------------------------------------

    TEST(SpeedMeterTest, InsideAWindowThePreviousSpeedIsKept) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(1000, t0 + 1s), 1000u);
        EXPECT_EQ(meter.update(1200, t0 + 1500ms), 1000u) << "0.5 s after the last point: no new value";
    }

    TEST(SpeedMeterTest, EachWindowIsMeasuredFromTheLastPoint) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(1000, t0 + 1s), 1000u);
        EXPECT_EQ(meter.update(1000 + 4000, t0 + 2s), 4000u)
                << "speed of the last second, not the average since the start (2500)";
    }

    TEST(SpeedMeterTest, PointInsideAWindowDoesNotMoveTheWindow) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(1000, t0 + 1s), 1000u);
        ASSERT_EQ(meter.update(1500, t0 + 1500ms), 1000u);   // ignored: inside the window
        EXPECT_EQ(meter.update(3000, t0 + 2s), 2000u)
                << "the window runs from the point at 1 s, not from the ignored one at 1.5 s";
    }

    TEST(SpeedMeterTest, NoNewBytesForAWindowIsZero) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(1000, t0 + 1s), 1000u);
        EXPECT_EQ(meter.update(1000, t0 + 2s), 0u) << "a stalled download has speed 0";
    }

    // --- restarts ----------------------------------------------------------------------------

    TEST(SpeedMeterTest, FewerBytesThanBeforeStartsAgain) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(5000, t0 + 1s), 5000u);
        EXPECT_EQ(meter.update(10, t0 + 2s), 0u) << "a new attempt from zero must not wrap around";
        EXPECT_EQ(meter.update(10 + 700, t0 + 3s), 700u) << "measuring continues from the restart";
    }

    TEST(SpeedMeterTest, StartResetsTheSpeed) {
        SpeedMeter meter = started_at_zero();
        ASSERT_EQ(meter.update(1000, t0 + 1s), 1000u);

        meter.start(t0 + 5s);

        EXPECT_EQ(meter.current_speed(), 0u);
        EXPECT_EQ(meter.update(300, t0 + 6s), 300u) << "measured from the new start point";
    }

    TEST(SpeedMeterTest, InitialBytesOfStartAreNotCountedAsSpeed) {
        SpeedMeter meter;
        meter.start(t0, 10'000);   // e.g. resuming a .part file that already has 10 000 bytes

        EXPECT_EQ(meter.update(10'000 + 500, t0 + 1s), 500u)
                << "only bytes received after start() count";
    }
}
